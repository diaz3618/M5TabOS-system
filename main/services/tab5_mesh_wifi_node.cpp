// Networks of Wi-Fi of no - see tab5_mesh_wifi_node.hpp.
#include "tab5_mesh_wifi_node.hpp"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "tab5_sd.hpp"
#include "tab5_wifi.hpp"

static const char* TAG = "mesh_wifi_no";
#define PASTA "Meshtastic"
#define ARQ   "wifi_no.txt"

EXT_RAM_BSS_ATTR static mesh_wifi_rede_t s_r[MESH_WIFI_REDES_MAX];
static int      s_n = 0;
static uint32_t s_versao = 0;
static bool     s_carregado = false;
static volatile bool s_gravar_pend = false;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Runs in task "cartao" (ride): rewrites the file when something changes.
static void in_carona(void)
{
  // The list is only rewritten after reading the card (see mesh_wifi_no_save).
  if (!s_carregado) { mesh_wifi_node_load(); return; }
  if (!s_gravar_pend) { return; }
  s_gravar_pend = false;
  // Copies of list with the lock and writes outside of it (card is slow).
  static mesh_wifi_rede_t copia[MESH_WIFI_REDES_MAX];
  int n;
  taskENTER_CRITICAL(&s_mux);
  n = s_n;
  memcpy(copia, s_r, sizeof(copia));
  taskEXIT_CRITICAL(&s_mux);
  // The entire file in memory is exchanged by the system port
  // (tab5_sd_replace): never middle file, and with the card borrowed on
  // computer goes to the diary. CONFIDENTIAL: the passwords of the old file are
  // reset before exit.
  const size_t cap = 64 + (size_t)MESH_WIFI_REDES_MAX * (sizeof(copia[0].ssid) + TAB5_WIFI_CIPHER_HEX + 2);
  char* txt = (char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (!txt) { s_gravar_pend = true; memset(copia, 0, sizeof(copia)); return; }
  size_t k = (size_t)snprintf(txt, cap, "# Meshtastic Wi-Fi networks (encrypted passwords)\n");
  for (int i = 0; i < n && k < cap; ++i) {
    char hex[TAB5_WIFI_CIPHER_HEX];
    if (copia[i].open) { snprintf(hex, sizeof(hex), "-"); }
    else if (!tab5_wifi_encrypt_text(copia[i].senha, hex, sizeof(hex))) { continue; }
    k += (size_t)snprintf(txt + k, cap - k, "%s\n%s\n", copia[i].ssid, hex);
  }
  if (!tab5_sd_replace(PASTA, ARQ, txt, k < cap ? k : cap - 1, TAB5_SD_SIGILOSO)) {
    s_gravar_pend = true;   // without card: try again in next iteration
  }
  heap_caps_free(txt);
  memset(copia, 0, sizeof(copia));   // text passwords: no copy left in RAM
}

void mesh_wifi_node_load(void)
{
  if (s_carregado) { return; }
  static bool carona = false;
  if (!carona) { carona = tab5_sd_piggyback(in_carona); }
  // Without card DOES NOT mark as loaded: the empty list of RAM cannot turn
  // the file. Try the ride again, until the card appears.
  if (!tab5_sd_mount()) { return; }
  FILE* f = tab5_sd_open(PASTA, ARQ, "r");
  if (f) {
    char ssid[48], hex[TAB5_WIFI_CIPHER_HEX + 8];
    int n = 0;
    taskENTER_CRITICAL(&s_mux);
    s_n = 0;
    taskEXIT_CRITICAL(&s_mux);
    while (n < MESH_WIFI_REDES_MAX && fgets(ssid, sizeof(ssid), f)) {
      ssid[strcspn(ssid, "\r\n")] = 0;
      if (ssid[0] == 0 || ssid[0] == '#') { continue; }
      if (!fgets(hex, sizeof(hex), f)) { break; }
      hex[strcspn(hex, "\r\n")] = 0;
      mesh_wifi_rede_t r;
      memset(&r, 0, sizeof(r));
      snprintf(r.ssid, sizeof(r.ssid), "%s", ssid);
      if (strcmp(hex, "-") == 0) { r.open = true; }
      else if (!tab5_wifi_decrypt_text(hex, r.senha, sizeof(r.senha))) { continue; }   // other device: cannot decrypt
      taskENTER_CRITICAL(&s_mux);
      s_r[s_n++] = r;
      taskEXIT_CRITICAL(&s_mux);
      ++n;
    }
    fclose(f);
  }
  tab5_sd_unmount();
  s_carregado = true;
  ++s_versao;
  ESP_LOGI(TAG, "%d rede(s) guardada(s) para o no", s_n);
}

int mesh_wifi_no_n(void) { return s_n; }

bool mesh_wifi_no_rede(int i, mesh_wifi_rede_t* o)
{
  bool ok = false;
  taskENTER_CRITICAL(&s_mux);
  if (i >= 0 && i < s_n) { *o = s_r[i]; ok = true; }
  taskEXIT_CRITICAL(&s_mux);
  return ok;
}

// Put 'r' at the top, removing the previous one of the same name. With the lock taken.
static void topo_locked(const mesh_wifi_rede_t& r)
{
  int onde = -1;
  for (int i = 0; i < s_n; ++i) { if (strcmp(s_r[i].ssid, r.ssid) == 0) { onde = i; break; } }
  if (onde < 0) {
    if (s_n < MESH_WIFI_REDES_MAX) { onde = s_n++; }
    else                           { onde = MESH_WIFI_REDES_MAX - 1; }   // full: exit oldest
  }
  for (int k = onde; k > 0; --k) { s_r[k] = s_r[k - 1]; }
  s_r[0] = r;
}

bool mesh_wifi_no_salvar(const char* ssid, const char* senha)
{
  if (!ssid || !ssid[0] || strlen(ssid) > 32 || (senha && strlen(senha) > 64)) { return false; }
  // THE CARD LIST FIRST: change an unread list and save it
  // would delete the saved networks. Without a card (or it on the computer), refuse.
  mesh_wifi_node_load();
  if (!s_carregado) { return false; }
  mesh_wifi_rede_t r;
  memset(&r, 0, sizeof(r));
  snprintf(r.ssid, sizeof(r.ssid), "%s", ssid);
  snprintf(r.senha, sizeof(r.senha), "%s", senha ? senha : "");
  r.open = (r.senha[0] == 0);
  taskENTER_CRITICAL(&s_mux);
  topo_locked(r);
  taskEXIT_CRITICAL(&s_mux);
  memset(&r, 0, sizeof(r));
  ++s_versao;
  s_gravar_pend = true;
  return true;
}

bool mesh_wifi_no_esquecer(int i)
{
  if (!s_carregado) { return false; }   // see mesh_wifi_no_save
  bool ok = false;
  taskENTER_CRITICAL(&s_mux);
  if (i >= 0 && i < s_n) {
    for (int k = i; k + 1 < s_n; ++k) { s_r[k] = s_r[k + 1]; }
    --s_n;
    memset(&s_r[s_n], 0, sizeof(s_r[0]));
    ok = true;
  }
  taskEXIT_CRITICAL(&s_mux);
  if (ok) { ++s_versao; s_gravar_pend = true; }
  return ok;
}

int mesh_wifi_no_trazer_of_tab5(void)
{
  if (tab5_wifi_known_count() == 0) { return -1; }
  int novas = 0;
  for (int i = 0; i < tab5_wifi_known_count(); ++i) {
    char ssid[33], senha[65];
    if (!tab5_wifi_saved_network(i, ssid, sizeof(ssid), senha, sizeof(senha))) { break; }
    bool ja = false;
    for (int k = 0; k < s_n; ++k) { if (strcmp(s_r[k].ssid, ssid) == 0) { ja = true; break; } }
    if (ja) { continue; }
    if (s_n >= MESH_WIFI_REDES_MAX) { break; }   // full: does not expel as of no
    mesh_wifi_no_salvar(ssid, senha);
    ++novas;
  }
  return novas;
}

uint32_t mesh_wifi_no_versao(void) { return s_versao; }
