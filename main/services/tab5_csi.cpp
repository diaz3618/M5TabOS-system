#include "tab5_csi.hpp"
#include "tab5_listener.hpp"
#include "tab5_sniffer.hpp"     // tab5_farejo_caps_c6: what the recorded C6 can do
#include "tab5_wifi.hpp"
#include "task_pool.h"
#include "service_stop.hpp"      // request/confirm task exit (same mechanism as GPS and Mesh)
#include "tab5_mutex.hpp"
#include "eh_host_feat_peer_data.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ping/ping_sock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>
#include <cstring>

static const char* TAG = "csi";

// The C6 disables CSI after C6_RADIO_VALIDADE_MS without renewal.
#define RENOVA_MS      3000
// Source of traffic: DOUBLE the rhythm request on C6 (1 record every 100 ms).
// With the source at the same rate of limit, the variable delay of air makes frames
// fall within 100 ms of the previous one and are discarded.
#define PING_MS        50
#define CSI_PASSO_MS   100

// -------------------------------------------------------------------------------
// THE MOVEMENT MEASUREMENT
// -------------------------------------------------------------------------------
// Only uses L-LTF, the part that every OFDM frame brings (11g and HT) and that comes
// first in buffer: 64 subcarriers, 2 bytes each, in order 0..31, -32..-1.
// They are worth +-1..+-26; the rest and DC and band of guard (almost zero, and normalize
// would divide noise by noise). Valid indexes in the buffer: 1..26 and 38..63.
//
// 1. Amplitude per subcarrier, NORMALIZED by the average of frame: the gain
// automatic (AGC) of C6 changes from frame to frame and would move everything together.
// 2. Deviation = average of |a - ref| over the 52 subcarriers, where ref is the average
// movement of own channel (~1 s). When stopped, the channel is stable and the deviation is only
// noise; something moving changes the default of reflections and the deviation goes up.
// 3. The "stop" of each place is different (walls, distance from the router): the
// service LEARNS the typical deviation stopped in the first seconds and then the
// goes slowly, just bottoms quickly. The exit level of RAZAO between the
// deviation now and the of stopped - one number that is valid in any house.
#define SC_N           52
#define REF_ALFA       0.10f    // moving average of channel: ~1 s at 10 records/s
#define DESV_ALFA      0.30f    // smoothes the deviation (~0.3 s)
#define APRENDER_N     30       // ~3 s of records for the first "stopped"
#define BASE_DESCE     0.20f    // the still moves quickly to the bottom...
#define BASE_SOBE      0.003f   // ...and slowly toward the top (~30 s): long movement does not become "normal" immediately
#define BASE_MIN       0.004f
#define RAZAO_LEVE     2.0f
#define RAZAO_FORTE    4.0f
#define HISTERESE      0.85f    // for lower of level the ratio drops 15% below the threshold

static uint8_t s_idx[SC_N];

// State of measurement: only the task of events (the callback) writes.
static float    s_ref[SC_N];
static bool     s_tem_ref = false;
static float    s_desv = 0, s_base = 0, s_base_soma = 0;
static int      s_aprendidos = 0;
static volatile float   s_razao = 1.0f;
static volatile uint8_t s_level = TAB5_CSI_PARADO;
static volatile int8_t  s_rssi = 0;
static volatile uint32_t s_cont = 0;
static volatile uint16_t s_por_s = 0;
static int64_t  s_t_seg = 0;
static uint32_t s_cont_seg = 0;

static volatile bool s_mexendo = false;
static volatile bool s_reset = false;      // request of restarting the reference (another task)
static tab5_listener_t<tab5_csi_ouvinte_t> s_ouvinte;   // remove in-flight call hold

// Service.
static volatile uint32_t s_quem = 0;
// ON AND OFF, NO RACING. Before 's_rodando' only turned true INSIDE
// of task: two 'enable' in a row created two tasks, and enable right after
// of disable fell on a task that was already exiting. Now what about the para_t of
// kernel: disable (the last 'who') ASKS for exit; enable with one execution
// leaving WAIT it confirms (at the traffic light) and only then creates the new one. The two
// pass through the same lock.
static stop_t          s_parada = { };
static SemaphoreHandle_t s_trava_cmd = nullptr;
static volatile tab5_csi_fase_t s_fase = TAB5_CSI_DESLIGADO;
static pool_id_t         s_id = POOL_ID_EMPTY;
static bool              s_registrado = false;

static void medir(const c6_csi_cab_t& c, const uint8_t* d, uint16_t n)
{
  // Only 11g and HT: start with L-LTF. HE has another arrangement of subcarriers.
  const uint8_t fmt = C6_CSI_FORMATO(c.flags);
  if ((fmt != 1 && fmt != 2) || n < 128) { return; }

  if (s_reset || s_mexendo) {
    s_reset = false;
    s_tem_ref = false;
    s_desv = 0;
    return;
  }

  float a[SC_N];
  float soma = 0;
  const bool w0_ruim = (c.flags & C6_CSI_F_W0_RUIM) != 0;   // invalid subcarriers 0 and 1
  int validos = 0;
  for (int k = 0; k < SC_N; ++k) {
    const int i = s_idx[k];
    if (w0_ruim && i < 2) { a[k] = -1.0f; continue; }
    const float im = (float)(int8_t)d[2 * i], re = (float)(int8_t)d[2 * i + 1];
    a[k] = sqrtf(im * im + re * re);
    soma += a[k];
    ++validos;
  }
  if (validos < SC_N - 2 || soma < (float)validos) { return; }   // low-energy frame: do not measure
  const float m = soma / (float)validos;
  for (int k = 0; k < SC_N; ++k) { if (a[k] >= 0) { a[k] /= m; } }

  if (!s_tem_ref) {
    for (int k = 0; k < SC_N; ++k) { s_ref[k] = a[k] >= 0 ? a[k] : 1.0f; }
    s_tem_ref = true;
    return;
  }
  float desv = 0;
  for (int k = 0; k < SC_N; ++k) {
    if (a[k] < 0) { continue; }
    desv += fabsf(a[k] - s_ref[k]);
    s_ref[k] += REF_ALFA * (a[k] - s_ref[k]);
  }
  desv /= (float)validos;
  s_desv += DESV_ALFA * (desv - s_desv);

  if (s_aprendidos < APRENDER_N) {
    s_base_soma += s_desv;
    if (++s_aprendidos == APRENDER_N) {
      s_base = s_base_soma / (float)APRENDER_N;
      if (s_base < BASE_MIN) { s_base = BASE_MIN; }
    }
    return;
  }
  s_base += (s_desv < s_base ? BASE_DESCE : BASE_SOBE) * (s_desv - s_base);
  if (s_base < BASE_MIN) { s_base = BASE_MIN; }

  const float r = s_desv / s_base;
  s_razao = r;
  uint8_t nv = s_level;
  if      (r >= RAZAO_FORTE)                            { nv = TAB5_CSI_FORTE; }
  else if (r >= RAZAO_LEVE && nv == TAB5_CSI_PARADO)    { nv = TAB5_CSI_LEVE; }
  else if (nv == TAB5_CSI_FORTE && r < RAZAO_FORTE * HISTERESE) { nv = r >= RAZAO_LEVE ? TAB5_CSI_LEVE : TAB5_CSI_PARADO; }
  else if (nv == TAB5_CSI_LEVE  && r < RAZAO_LEVE * HISTERESE)  { nv = TAB5_CSI_PARADO; }
  s_level = nv;
}

// In the task of events of esp_hosted: measure and exit.
static void on_peer_data(uint32_t msg_id, const uint8_t* d, size_t n, void*)
{
  if (msg_id != C6_MSG_CSI || n < sizeof(c6_csi_cab_t)) { return; }
  c6_csi_cab_t c;
  memcpy(&c, d, sizeof(c));
  const uint8_t* bytes = d + sizeof(c);
  uint16_t len = (uint16_t)(n - sizeof(c));
  if (len > c.len) { len = c.len; }

  s_cont = s_cont + 1;
  s_rssi = c.rssi;
  const int64_t now = esp_timer_get_time();
  ++s_cont_seg;
  if (now - s_t_seg >= 1000000) { s_por_s = (uint16_t)s_cont_seg; s_cont_seg = 0; s_t_seg = now; }

  medir(c, bytes, len);
  tab5_listener_call(&s_ouvinte, (const c6_csi_cab_t*)&c, bytes, len);
}

// -------------------------------------------------------------------------------
// Renovation + source of traffic - vacancy of pool, only while enabled.
// -------------------------------------------------------------------------------
static void mandar(const c6_csi_cfg_t& cfg)
{
  uint8_t b[1 + sizeof(c6_csi_cfg_t)];
  b[0] = C6_RX_CSI;
  memcpy(b + 1, &cfg, sizeof(cfg));
  eh_host_peer_data_send(C6_MSG_HOST_RADIO, b, sizeof(b));
}

static esp_ping_handle_t ping_start(uint32_t gw)
{
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.target_addr.type = IPADDR_TYPE_V4;
  cfg.target_addr.u_addr.ip4.addr = gw;
  cfg.count = ESP_PING_COUNT_INFINITE;
  cfg.interval_ms = PING_MS;
  cfg.timeout_ms = 1000;
  cfg.data_size = 16;
  esp_ping_callbacks_t cb = { };
  esp_ping_handle_t h = nullptr;
  if (esp_ping_new_session(&cfg, &cb, &h) != ESP_OK) { return nullptr; }
  esp_ping_start(h);
  return h;
}

static void ping_stop(esp_ping_handle_t* h)
{
  if (!*h) { return; }
  esp_ping_stop(*h);
  esp_ping_delete_session(*h);
  *h = nullptr;
}

static void vTarefa(void*)
{
  esp_ping_handle_t ping = nullptr;
  uint32_t gw_ping = 0;
  bool pediu_c6 = false;
  tab5_wifi_keep_awake(TAB5_AWAKE_CSI, true);   // in modem sleep the C6 loses frames

  while (!stop_requested(&s_parada)) {
    // Is there where to measure? Our firmware with CSI, and WiFi connected to the router.
    const bool c6_ok = tab5_wifi_c6_is_ours() && tab5_wifi_c6_link()
                       && (tab5_farejo_caps_c6() & C6_CAP_CSI);
    uint32_t gw = 0;
    wifi_ap_record_t ap;
    bool tem_ap = false;
    if (c6_ok && tab5_wifi_state() == TAB5_WIFI_CONNECTED) {
      esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
      esp_netif_ip_info_t ip;
      if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK) { gw = ip.gw.addr; }
      tem_ap = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    }

    if (!c6_ok) {
      s_fase = TAB5_CSI_SEM_C6;
    } else if (!gw || !tem_ap) {
      s_fase = TAB5_CSI_SEM_ROTEADOR;
    }

    if (c6_ok && gw && tem_ap) {
      if (s_fase == TAB5_CSI_SEM_C6 || s_fase == TAB5_CSI_SEM_ROTEADOR) { s_fase = TAB5_CSI_APRENDENDO; }
      // Only the CSI that comes FROM THE ROUTER: and the fixed link that serves as reference.
      c6_csi_cfg_t cfg = { };
      cfg.liga = 1;
      cfg.so_alvo = 1;
      memcpy(cfg.alvo, ap.bssid, 6);
      cfg.min_intervalo_ms = CSI_PASSO_MS;
      mandar(cfg);
      pediu_c6 = true;
      if (gw != gw_ping) { ping_stop(&ping); gw_ping = 0; }
      if (!ping) { ping = ping_start(gw); if (ping) { gw_ping = gw; } }
    } else {
      if (pediu_c6) { c6_csi_cfg_t off = { }; mandar(off); pediu_c6 = false; }
      ping_stop(&ping);
      gw_ping = 0;
      s_reset = true;
    }

    for (int i = 0; i < RENOVA_MS / 100 && !stop_requested(&s_parada); ++i) { vTaskDelay(pdMS_TO_TICKS(100)); }
  }

  ping_stop(&ping);
  if (pediu_c6) { c6_csi_cfg_t off = { }; mandar(off); }
  tab5_wifi_keep_awake(TAB5_AWAKE_CSI, false);
  s_fase = TAB5_CSI_DESLIGADO;
  ESP_LOGI(TAG, "CSI disabled");
  stop_confirm(&s_parada);   // the LAST thing: from now on one enable can create another
}

// -------------------------------------------------------------------------------
// API
// -------------------------------------------------------------------------------
static void prepare(void)
{
  if (s_registrado) { return; }
  // The 52 valid indices of L-LTF in the buffer (see THE MOVEMENT MEASUREMENT).
  int k = 0;
  for (int i = 1; i <= 26; ++i)  { s_idx[k++] = (uint8_t)i; }
  for (int i = 38; i <= 63; ++i) { s_idx[k++] = (uint8_t)i; }
  eh_host_peer_data_register(C6_MSG_CSI, on_peer_data, nullptr);
  s_registrado = true;
}

void tab5_csi_enable(uint32_t quem)
{
  prepare();
  tab5_mutex_lock(&s_trava_cmd);
  if (stop_running(&s_parada)) {
    if (!stop_requested(&s_parada)) {     // of standing and staying: only add those who want
      s_quem = s_quem | quem;
      tab5_mutex_destravar(&s_trava_cmd);
      return;
    }
    // Leaving (the last one just hung up): wait for the exit, then go up another one.
    if (!stop_request(&s_parada, 1500)) {
      tab5_mutex_destravar(&s_trava_cmd);
      ESP_LOGE(TAG, "the previous CSI task has not exited - not enabled");
      return;
    }
  }
  s_quem = s_quem | quem;
  // Measure of zero: the "still" learned is valid for the place of before.
  s_reset = true;
  s_aprendidos = 0;
  s_base_soma = 0;
  s_base = 0;
  s_razao = 1.0f;
  s_level = TAB5_CSI_PARADO;
  s_fase = TAB5_CSI_APRENDENDO;
  if (!stop_prepare(&s_parada)
   || !pool_create(&s_id, vTarefa, nullptr, "csi", 3072, 3, OWNER_SERVICE, "CSI")) {
    stop_abandon(&s_parada);
    s_quem = 0;
    s_fase = TAB5_CSI_DESLIGADO;
    tab5_mutex_destravar(&s_trava_cmd);
    ESP_LOGE(TAG, "no slot for the CSI task");
    return;
  }
  tab5_mutex_destravar(&s_trava_cmd);
  ESP_LOGI(TAG, "CSI enabled");
}

void tab5_csi_disable(uint32_t quem)
{
  tab5_mutex_lock(&s_trava_cmd);
  s_quem = s_quem & ~quem;
  // The last requester exited: REQUEST task exit (do not wait—the screen
  // caller disables it). The task sees the request on the next iteration;
  // wake interrupts its vTaskDelay.
  if (!s_quem && stop_running(&s_parada)) { s_parada.request = true; pool_wake(&s_id); }
  tab5_mutex_destravar(&s_trava_cmd);
}

bool tab5_csi_active(void) { return s_quem != 0; }

void tab5_csi_aparelho_mexendo(bool sim) { s_mexendo = sim; }

void tab5_csi_ouvinte(tab5_csi_ouvinte_t cb)
{
  prepare();
  tab5_listener_set(&s_ouvinte, cb);
}

void tab5_csi_estado(tab5_csi_estado_t* out)
{
  if (!out) { return; }
  out->por_s = s_por_s;
  out->rssi = s_rssi;
  out->razao = s_razao;
  out->level = (tab5_csi_level_t)s_level;
  tab5_csi_fase_t f = s_fase;
  if (f == TAB5_CSI_APRENDENDO || f == TAB5_CSI_MEDINDO) {
    // The por_s is only rewritten when it arrives record: stopped for more than 2 s and the
    // same as zero.
    if (esp_timer_get_time() - s_t_seg > 2000000) { out->por_s = 0; }
    if (s_mexendo)                         { f = TAB5_CSI_APARELHO_MEXENDO; }
    else if (s_aprendidos < APRENDER_N)    { f = TAB5_CSI_APRENDENDO; }
    else                                   { f = TAB5_CSI_MEDINDO; }
  }
  out->fase = f;
}
