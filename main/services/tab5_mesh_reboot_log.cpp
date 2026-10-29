// Journal of restarts of no - see warning in tab5_mesh_reboot_log.hpp.
#include "tab5_mesh_reboot_log.hpp"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tab5_sd.hpp"
#include "tab5_power.hpp"
#include "tab5_mesh.hpp"
#include "tab5_time.hpp"   // tab5_time_epoch

static const char* TAG = "mesh_diario";

#define PASTA "Meshtastic"
#define ARQ   "reinicios.txt"
// The photo is valid up to here: after that the context is no longer the one of restart.
#define FOTO_VALE_MS 180000u
// Window of verdict: no takes ~5 s to raise and Tab5 notices soon after.
#define JANELA_S 30

EXT_RAM_BSS_ATTR static mesh_reinicio_t s_e[MESH_DIARIO_MAX];   // of oldest (0) on youngest
static int      s_n = 0;
static uint32_t s_ult_cont = 0;
static uint32_t s_versao = 0;
static bool     s_carregado = false;
static volatile bool s_limpar_pend = false;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t s_send_state_ms = 0;      // last send_state that was not beat
static uint8_t  s_send_state_tipo = 0;

typedef struct { bool tem; uint32_t ms; uint32_t up_no_s; } foto_t;
static foto_t s_foto;


void mesh_diario_nota_send_state(uint8_t tipo)
{
  if (tipo == MESH_ENV_HEARTBEAT || tipo == MESH_ENV_NENHUM) { return; }   // every 30 s: just noise
  s_send_state_ms = tab5_now_ms() | 1u;
  s_send_state_tipo = tipo;
}

static int32_t ha_s(uint32_t ms_of_evento)
{
  if (ms_of_evento == 0) { return -1; }
  return (int32_t)((tab5_now_ms() - ms_of_evento) / 1000u);
}

static void line_of(const mesh_reinicio_t* r, char* o, size_t n)
{
  snprintf(o, n, "%lu|%lu|%lu|%lu|%u|%u|%u|%ld|%ld|%ld|%ld\n",
           (unsigned long)r->epoch, (unsigned long)r->up_tab5_s, (unsigned long)r->contador,
           (unsigned long)r->up_no_s, (unsigned)r->origem, (unsigned)r->send_state_tipo,
           (unsigned)r->centivolts, (long)r->ma, (long)r->send_state_s, (long)r->grove_s, (long)r->profile_s);
}

static bool le_line(const char* l, mesh_reinicio_t* r)
{
  unsigned long e, ut, c, un; unsigned o, et, cv; long ma, es, gs, ps;
  if (sscanf(l, "%lu|%lu|%lu|%lu|%u|%u|%u|%ld|%ld|%ld|%ld", &e, &ut, &c, &un, &o, &et, &cv, &ma, &es, &gs, &ps) != 11) { return false; }
  r->epoch = e; r->up_tab5_s = ut; r->contador = c; r->up_no_s = un; r->origem = (uint8_t)o;
  r->send_state_tipo = (uint8_t)et; r->centivolts = (uint16_t)cv; r->ma = ma; r->send_state_s = es; r->grove_s = gs; r->profile_s = ps;
  return true;
}

static void juntar(const mesh_reinicio_t& r)
{
  taskENTER_CRITICAL(&s_mux);
  if (s_n >= MESH_DIARIO_MAX) { memmove(&s_e[0], &s_e[1], sizeof(s_e[0]) * (MESH_DIARIO_MAX - 1)); s_n = MESH_DIARIO_MAX - 1; }
  s_e[s_n++] = r;
  taskEXIT_CRITICAL(&s_mux);
  ++s_versao;
}

// Runs in "card" task: deletes the file when the screen requests it.
static void in_carona(void)
{
  if (!s_limpar_pend) { return; }
  s_limpar_pend = false;
  // Empty file through the system port: with the borrowed card, go to the
  // diary - and the replace writes the lines that were about to enter beforehand.
  if (!tab5_sd_replace(PASTA, ARQ, "", 0)) { s_limpar_pend = true; }
}

void mesh_diario_carregar(void)
{
  if (s_carregado) { return; }
  static bool carona = false;
  if (!carona) { carona = tab5_sd_piggyback(in_carona); }
  if (!tab5_sd_mount()) { s_carregado = true; return; }   // without card: just the RAM for this session
  FILE* f = tab5_sd_open(PASTA, ARQ, "r");
  if (f) {
    char l[160];
    mesh_reinicio_t r;
    while (fgets(l, sizeof(l), f)) {
      if (!le_line(l, &r)) { continue; }
      juntar(r);
      if (r.contador) { s_ult_cont = r.contador; }
    }
    fclose(f);
  }
  tab5_sd_unmount();
  s_carregado = true;
  ESP_LOGI(TAG, "diario: %d registro(s), ultimo contador %lu", s_n, (unsigned long)s_ult_cont);
}

void mesh_diario_foto(uint32_t up_no_s)
{
  s_foto.tem = true; s_foto.ms = tab5_now_ms() | 1u; s_foto.up_no_s = up_no_s;
  // The context of Tab5 is read at the time of REGISTRATION, but the event time is
  // related to it: the photo only marks when the notice arrived.
}

void mesh_diario_contador(uint32_t contador, uint32_t up_no_s, bool conectados_antes, bool request_nosso)
{
  if (contador == 0 || !s_carregado) { return; }
  const uint32_t ant = s_ult_cont;
  if (contador == ant) { return; }

  mesh_reinicio_t r;
  memset(&r, 0, sizeof(r));
  r.epoch = tab5_time_epoch();
  r.up_tab5_s = tab5_now_ms() / 1000u;
  r.contador = contador;

  const bool foto_fresca = s_foto.tem && (tab5_now_ms() - s_foto.ms) < FOTO_VALE_MS;
  // Uptime of BEFORE falling: the photo (taken on arrival, before
  // new telemetry) or, without a photo, what the service still keeps.
  r.up_no_s = foto_fresca ? s_foto.up_no_s : up_no_s;

  if (ant == 0 || contador < ant) { r.origem = MESH_REI_BASE; }   // without reference, or the no was reset/rewritten
  else if (request_nosso)          { r.origem = MESH_REI_PEDIDO; }
  else if (!conectados_antes)     { r.origem = MESH_REI_AUSENTE; }
  else                            { r.origem = MESH_REI_SOZINHO; }

  const tab5_measurement_t m = tab5_power_latest();
  r.centivolts = (uint16_t)(m.volts > 0 ? m.volts * 100.0f : 0);
  r.ma = (int32_t)m.mA;
  r.send_state_s = ha_s(s_send_state_ms);
  r.send_state_tipo = s_send_state_tipo;
  r.grove_s = ha_s(tab5_power_grove_touch_ms());
  r.profile_s = ha_s(tab5_power_apply_ms());
  // The warning arrived "photo" after the restart: the times above count since the
  // REGISTRATION; removes the interval already elapsed so that they are relative to the restart.
  if (foto_fresca) {
    const int32_t atraso = (int32_t)((tab5_now_ms() - s_foto.ms) / 1000u);
    if (r.send_state_s >= 0) { r.send_state_s = r.send_state_s > atraso ? r.send_state_s - atraso : 0; }
    if (r.grove_s >= 0) { r.grove_s = r.grove_s > atraso ? r.grove_s - atraso : 0; }
    if (r.profile_s >= 0) { r.profile_s = r.profile_s > atraso ? r.profile_s - atraso : 0; }
  }
  s_foto.tem = false;
  s_ult_cont = contador;
  juntar(r);

  char line[160];
  line_of(&r, line, sizeof(line));
  tab5_sd_enqueue_append(PASTA, ARQ, line);
  ESP_LOGW(TAG, "node rebooted (counter %lu -> %lu, source %u): %s", (unsigned long)ant, (unsigned long)contador,
           (unsigned)r.origem, mesh_diario_veredito(&r));
}

int mesh_diario_read(mesh_reinicio_t* o, int max)
{
  int k = 0;
  taskENTER_CRITICAL(&s_mux);
  for (int i = s_n - 1; i >= 0 && k < max; --i) { o[k++] = s_e[i]; }
  taskEXIT_CRITICAL(&s_mux);
  return k;
}

uint32_t mesh_diario_versao(void) { return s_versao; }

void mesh_diario_limpar(void)
{
  taskENTER_CRITICAL(&s_mux);
  s_n = 0;
  taskEXIT_CRITICAL(&s_mux);
  s_limpar_pend = true;
  ++s_versao;
  // the counter of reference stays: erase the diary does not make the next restart become "base"
}

const char* mesh_diario_veredito(const mesh_reinicio_t* r)
{
  switch (r->origem) {
    case MESH_REI_BASE:    return "ponto de partida (primeira leitura)";
    case MESH_REI_PEDIDO:  return "requested by the Tab5 (save configuration or reboot)";
    case MESH_REI_AUSENTE: return "rebooted while the Tab5 was disconnected - timing unknown";
    default: break;
  }
  if (r->grove_s >= 0 && r->grove_s <= JANELA_S) { return "the Tab5 changed Grove 5 V at that time - probable cause"; }
  if (r->profile_s >= 0 && r->profile_s <= JANELA_S) { return "o Tab5 reaplicou o perfil de energia na hora - suspeito"; }
  if (r->send_state_s >= 0 && r->send_state_s <= 10) {
    return r->send_state_tipo == MESH_ENV_ADMIN ? "logo depois de um comando de admin do Tab5 - suspeito"
         : r->send_state_tipo == MESH_ENV_CONFIG ? "right after the Tab5 configuration request - suspicious"
         : "logo depois de um pacote enviado pelo Tab5 - suspeito";
  }
  if (r->centivolts && r->centivolts < 650) { return "Tab5 battery low at that time - check the power supply"; }
  return "no evidence on the Tab5 side: node failure (module firmware or power)";
}
