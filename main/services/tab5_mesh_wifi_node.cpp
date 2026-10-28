// Redes do Wi-Fi do no - ver tab5_mesh_wifi_node.hpp.
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

// Roda na tarefa "cartao" (carona): reescreve o arquivo quando algo mudou.
static void na_carona(void)
{
  // A lista so e reescrita depois de lida do cartao (ver mesh_wifi_no_salvar).
  if (!s_carregado) { mesh_wifi_no_carregar(); return; }
  if (!s_gravar_pend) { return; }
  s_gravar_pend = false;
  // Copia da lista com a trava e escreve fora dela (cartao e lento).
  static mesh_wifi_rede_t copia[MESH_WIFI_REDES_MAX];
  int n;
  taskENTER_CRITICAL(&s_mux);
  n = s_n;
  memcpy(copia, s_r, sizeof(copia));
  taskEXIT_CRITICAL(&s_mux);
  // O arquivo inteiro na memoria e UMA troca pela porta do sistema
  // (tab5_sd_substituir): nunca meio arquivo, e com o cartao emprestado ao
  // computador vai para o diario. SIGILOSO: as senhas do arquivo antigo sao
  // zeradas antes de ele sair.
  const size_t cap = 64 + (size_t)MESH_WIFI_REDES_MAX * (sizeof(copia[0].ssid) + TAB5_WIFI_CIFRA_HEX + 2);
  char* txt = (char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (!txt) { s_gravar_pend = true; memset(copia, 0, sizeof(copia)); return; }
  size_t k = (size_t)snprintf(txt, cap, "# redes do Wi-Fi do no Meshtastic (senhas cifradas)\n");
  for (int i = 0; i < n && k < cap; ++i) {
    char hex[TAB5_WIFI_CIFRA_HEX];
    if (copia[i].aberta) { snprintf(hex, sizeof(hex), "-"); }
    else if (!tab5_wifi_cifrar_texto(copia[i].senha, hex, sizeof(hex))) { continue; }
    k += (size_t)snprintf(txt + k, cap - k, "%s\n%s\n", copia[i].ssid, hex);
  }
  if (!tab5_sd_substituir(PASTA, ARQ, txt, k < cap ? k : cap - 1, TAB5_SD_SIGILOSO)) {
    s_gravar_pend = true;   // sem cartao: tenta de novo na proxima volta
  }
  heap_caps_free(txt);
  memset(copia, 0, sizeof(copia));   // senhas em texto: nao deixa copia na RAM
}

void mesh_wifi_no_carregar(void)
{
  if (s_carregado) { return; }
  static bool carona = false;
  if (!carona) { carona = tab5_sd_carona(na_carona); }
  // Sem cartao NAO marca como carregado: a lista vazia da RAM nao pode virar
  // o arquivo. Tenta de novo na carona, ate o cartao aparecer.
  if (!tab5_sd_montar()) { return; }
  FILE* f = tab5_sd_abrir(PASTA, ARQ, "r");
  if (f) {
    char ssid[48], hex[TAB5_WIFI_CIFRA_HEX + 8];
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
      if (strcmp(hex, "-") == 0) { r.aberta = true; }
      else if (!tab5_wifi_decifrar_texto(hex, r.senha, sizeof(r.senha))) { continue; }   // outro aparelho: nao decifra
      taskENTER_CRITICAL(&s_mux);
      s_r[s_n++] = r;
      taskEXIT_CRITICAL(&s_mux);
      ++n;
    }
    fclose(f);
  }
  tab5_sd_desmontar();
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

// Poe 'r' no topo, removendo a anterior de mesmo nome. Com a trava tomada.
static void topo_locked(const mesh_wifi_rede_t& r)
{
  int onde = -1;
  for (int i = 0; i < s_n; ++i) { if (strcmp(s_r[i].ssid, r.ssid) == 0) { onde = i; break; } }
  if (onde < 0) {
    if (s_n < MESH_WIFI_REDES_MAX) { onde = s_n++; }
    else                           { onde = MESH_WIFI_REDES_MAX - 1; }   // cheia: sai a mais antiga
  }
  for (int k = onde; k > 0; --k) { s_r[k] = s_r[k - 1]; }
  s_r[0] = r;
}

bool mesh_wifi_no_salvar(const char* ssid, const char* senha)
{
  if (!ssid || !ssid[0] || strlen(ssid) > 32 || (senha && strlen(senha) > 64)) { return false; }
  // A LISTA DO CARTAO PRIMEIRO: mudar uma lista que nao foi lida e grava-la
  // apagaria as redes guardadas. Sem cartao (ou ele no computador), recusa.
  mesh_wifi_no_carregar();
  if (!s_carregado) { return false; }
  mesh_wifi_rede_t r;
  memset(&r, 0, sizeof(r));
  snprintf(r.ssid, sizeof(r.ssid), "%s", ssid);
  snprintf(r.senha, sizeof(r.senha), "%s", senha ? senha : "");
  r.aberta = (r.senha[0] == 0);
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
  if (!s_carregado) { return false; }   // ver mesh_wifi_no_salvar
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

int mesh_wifi_no_trazer_do_tab5(void)
{
  if (tab5_wifi_n_conhecidas() == 0) { return -1; }
  int novas = 0;
  for (int i = 0; i < tab5_wifi_n_conhecidas(); ++i) {
    char ssid[33], senha[65];
    if (!tab5_wifi_rede_guardada(i, ssid, sizeof(ssid), senha, sizeof(senha))) { break; }
    bool ja = false;
    for (int k = 0; k < s_n; ++k) { if (strcmp(s_r[k].ssid, ssid) == 0) { ja = true; break; } }
    if (ja) { continue; }
    if (s_n >= MESH_WIFI_REDES_MAX) { break; }   // cheia: nao expulsa as do no
    mesh_wifi_no_salvar(ssid, senha);
    ++novas;
  }
  return novas;
}

uint32_t mesh_wifi_no_versao(void) { return s_versao; }
