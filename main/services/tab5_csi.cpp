#include "tab5_csi.hpp"
#include "tab5_listener.hpp"
#include "tab5_sniffer.hpp"     // tab5_farejo_caps_c6: o que o C6 gravado sabe fazer
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
// Fonte of trafego: o DOBRO of ritmo request on C6 (1 record a cada 100 ms).
// Com a fonte no mesmo ritmo of limit, o atraso variavel of ar faz quadros
// cairem a menos of 100 ms of anterior e serem descartados.
#define PING_MS        50
#define CSI_PASSO_MS   100

// ---------------------------------------------------------------------------
// A MEDIDA DE MOVIMENTO
// ---------------------------------------------------------------------------
// Usa so o L-LTF, a parte que todo frame OFDM traz (11g e HT) e que vem
// first no buffer: 64 subportadoras, 2 bytes cada, in order 0..31, -32..-1.
// Valem +-1..+-26; o resto e DC e banda of guarda (quase zero, e normalizar
// dividiria ruido por ruido). Indices validos no buffer: 1..26 e 38..63.
//
// 1. Amplitude por subportadora, NORMALIZADA pela media of frame: o ganho
//    automatico (AGC) of C6 muda of frame para frame e moveria everything junto.
// 2. Desvio = media of |a - ref| sobre as 52 subportadoras, onde ref e a media
//    movel of proprio canal (~1 s). Parado, o canal e estavel e o desvio e so
//    ruido; algo se mexendo muda o default of reflexos e o desvio sobe.
// 3. O "parado" of cada lugar e diferente (paredes, distancia on roteador): o
//    servico APRENDE o desvio tipico parado nos primeiros segundos e depois o
//    segue devagar, so para bottom depressa. O level exit of RAZAO entre o
//    desvio now e o of parado - one numero que vale em qualquer casa.
#define SC_N           52
#define REF_ALFA       0.10f    // media movel of canal: ~1 s a 10 records/s
#define DESV_ALFA      0.30f    // suaviza o desvio (~0,3 s)
#define APRENDER_N     30       // ~3 s of records para o first "parado"
#define BASE_DESCE     0.20f    // o parado acompanha depressa para bottom...
#define BASE_SOBE      0.003f   // ...and slowly toward the top (~30 s): long movement does not become "normal" immediately
#define BASE_MIN       0.004f
#define RAZAO_LEVE     2.0f
#define RAZAO_FORTE    4.0f
#define HISTERESE      0.85f    // para lower of level a razao cai 15% abaixo of limiar

static uint8_t s_idx[SC_N];

// Estado of measurement: so a tarefa of eventos (o callback) escreve.
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
static volatile bool s_reset = false;      // request of recomecar a referencia (outra tarefa)
static tab5_listener_t<tab5_csi_ouvinte_t> s_ouvinte;   // remover espera a chamada em voo

// Servico.
static volatile uint32_t s_quem = 0;
// LIGAR E DESLIGAR, SEM CORRIDA. Antes 's_rodando' so virava true DENTRO
// of tarefa: dois 'enable' seguidos criavam duas tarefas, e enable logo depois
// of disable caia numa tarefa que ja estava saindo. Agora e o parada_t of
// kernel: disable (o ultimo 'quem') PEDE a saida; enable com uma execucao
// saindo ESPERA ela confirm (no semaforo) e so entao cria a nova. Os dois
// passam pela mesma trava.
static stop_t          s_parada = { };
static SemaphoreHandle_t s_trava_cmd = nullptr;
static volatile tab5_csi_fase_t s_fase = TAB5_CSI_DESLIGADO;
static pool_id_t         s_id = POOL_ID_EMPTY;
static bool              s_registrado = false;

static void medir(const c6_csi_cab_t& c, const uint8_t* d, uint16_t n)
{
  // So 11g e HT: comecam pelo L-LTF. HE tem outro arranjo of subportadoras.
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
  const bool w0_ruim = (c.flags & C6_CSI_F_W0_RUIM) != 0;   // subportadoras 0 e 1 invalidas
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

// Na tarefa of eventos of esp_hosted: mede e exit.
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

// ---------------------------------------------------------------------------
// Renovacao + fonte of trafego - vaga of pool, so enquanto enabled.
// ---------------------------------------------------------------------------
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
  tab5_wifi_keep_awake(TAB5_AWAKE_CSI, true);   // em modem sleep o C6 perde quadros

  while (!stop_requested(&s_parada)) {
    // Ha of onde medir? Firmware nosso com CSI, e WiFi conectado on roteador.
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
      // So o CSI que vem DO ROTEADOR: e o elo fixo que serve of referencia.
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
  stop_confirm(&s_parada);   // a ULTIMA coisa: daqui em diante one enable pode create outra
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
static void prepare(void)
{
  if (s_registrado) { return; }
  // Os 52 indices validos of L-LTF no buffer (ver A MEDIDA DE MOVIMENTO).
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
    if (!stop_requested(&s_parada)) {     // of pe e ficando: so soma quem quer
      s_quem = s_quem | quem;
      tab5_mutex_destravar(&s_trava_cmd);
      return;
    }
    // Saindo (o ultimo desligou ha pouco): espera a saida, depois sobe outra.
    if (!stop_request(&s_parada, 1500)) {
      tab5_mutex_destravar(&s_trava_cmd);
      ESP_LOGE(TAG, "the previous CSI task has not exited - not enabled");
      return;
    }
  }
  s_quem = s_quem | quem;
  // Medida of zero: o "parado" aprendido vale para o lugar of antes.
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
    // O por_s so e reescrito when chega record: parado ha mais of 2 s e o
    // mesmo que zero.
    if (esp_timer_get_time() - s_t_seg > 2000000) { out->por_s = 0; }
    if (s_mexendo)                         { f = TAB5_CSI_APARELHO_MEXENDO; }
    else if (s_aprendidos < APRENDER_N)    { f = TAB5_CSI_APRENDENDO; }
    else                                   { f = TAB5_CSI_MEDINDO; }
  }
  out->fase = f;
}
