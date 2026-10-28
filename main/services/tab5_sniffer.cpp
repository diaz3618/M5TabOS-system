#include "tab5_sniffer.hpp"
#include "tab5_listener.hpp"
#include "tab5_wifi.hpp"
#include "tab5_time.hpp"      // tab5_now_ms
#include "task_pool.h"
#include "service_stop.hpp"      // request/confirm a saida of tarefa (o mesmo of GPS e of Mesh)
#include "tab5_mutex.hpp"
#include "eh_host_feat_peer_data.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <cstring>

static const char* TAG = "farejo";

// O C6 desliga sozinho em C6_FAREJO_VALIDADE_MS sem renovacao. Renovar a cada
// 3 s aguenta dois pedidos perdidos seguidos.
#define RENOVA_MS 3000

static StaticSemaphore_t s_mtx_buf;
static SemaphoreHandle_t s_mtx = nullptr;
static StaticSemaphore_t s_info_buf;
static SemaphoreHandle_t s_info_sem = nullptr;

static c6_farejo_cfg_t s_cfg;          // o request em vigor (sob s_mtx)
static volatile bool   s_quer = false; // o P4 quer farejar
// LIGAR E DESLIGAR, SEM CORRIDA (o mesmo of CSI). Antes, religar logo
// depois of disable achava 's_rodando' ainda of pe, nao voltava s_quer a
// true, e a tarefa que estava saindo desligava o farejo e LIBERAVA A TABELA
// depois of 'enable' ter respondido true. Agora disable PEDE a saida
// (parada_t) e enable com uma execucao saindo espera ela confirm antes of
// alocar a tabela e create a nova.
static stop_t          s_parada = { };
static SemaphoreHandle_t s_trava_cmd = nullptr;
static pool_id_t       s_id = POOL_ID_EMPTY;

static farejo_item_t* s_tab = nullptr; // PSRAM, so com o farejo enabled
static int            s_n = 0;
static volatile uint32_t s_lotes = 0, s_perdidos = 0;

static volatile uint8_t s_proto = 0, s_caps = 0;   // 0 = ainda nao sabemos
static c6_info_t s_info;
static volatile bool s_tem_info = false;
static bool s_registrado = false;

static void start_travas(void)
{
  if (!s_mtx) {
    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
    s_info_sem = xSemaphoreCreateBinaryStatic(&s_info_buf);
  }
}

// ---------------------------------------------------------------------------
// Vindo of C6 - in tarefa of eventos of esp_hosted: so junta e exit.
// ---------------------------------------------------------------------------
static int achar_mac(const uint8_t mac[6])   // sob s_mtx
{
  for (int i = 0; i < s_n; ++i) {
    if (memcmp(s_tab[i].mac, mac, 6) == 0) { return i; }
  }
  return -1;
}

static int vaga_para(const uint8_t mac[6])   // sob s_mtx; cheio: o mais antigo
{
  int j = achar_mac(mac);
  if (j >= 0) { return j; }
  if (s_n < FAREJO_MAX) { return s_n++; }
  int pior = 0;
  for (int i = 1; i < s_n; ++i) {
    if (s_tab[i].seen_ms < s_tab[pior].seen_ms) { pior = i; }
  }
  memset(&s_tab[pior], 0, sizeof(s_tab[pior]));
  return pior;
}

static tab5_listener_t<tab5_farejo_ouvinte_t> s_ouvinte;   // remover espera a chamada em voo

static void juntar_lote(const uint8_t* d, size_t n)
{
  if (n < sizeof(c6_farejo_lote_t)) { return; }
  c6_farejo_lote_t cab;
  memcpy(&cab, d, sizeof(cab));
  const uint8_t* reg = d + sizeof(cab);
  int k = (int)((n - sizeof(cab)) / sizeof(c6_farejo_reg_t));
  if (k > cab.n) { k = cab.n; }
  const uint32_t now = tab5_now_ms();

  // O lote chega when o periodo FECHA: o RSSI medio dele e of middle of
  // periodo. (O caminho C6 -> P4 leva poucos ms; o periodo leva 100+.)
  if (s_ouvinte.fn.load()) {
    uint16_t lote_ms = s_cfg.lote_ms ? s_cfg.lote_ms : 250;
    if (lote_ms < 50) { lote_ms = 50; }
    const int64_t t = esp_timer_get_time() - (int64_t)lote_ms * 500;
    for (int i = 0; i < k; ++i) {
      c6_farejo_reg_t g;
      memcpy(&g, reg + i * sizeof(g), sizeof(g));
      tab5_listener_call(&s_ouvinte, (const c6_farejo_reg_t*)&g, t);
    }
  }

  xSemaphoreTake(s_mtx, portMAX_DELAY);
  if (s_tab) {
    for (int i = 0; i < k; ++i) {
      c6_farejo_reg_t g;
      memcpy(&g, reg + i * sizeof(g), sizeof(g));   // reg pode estar desalinhado
      int j = vaga_para(g.mac);
      farejo_item_t* it = &s_tab[j];
      memcpy(it->mac, g.mac, 6);
      it->rssi_ult = g.rssi_ult;
      it->rssi_max = g.rssi_max;
      it->rssi_med = g.rssi_med;
      it->tipos   |= g.tipos;
      it->channel    = g.channel;
      it->frames += g.n;
      it->seen_ms = now;
    }
    s_perdidos += cab.perdidos;
  }
  xSemaphoreGive(s_mtx);
  s_lotes = s_lotes + 1;   // volatile: nao usar ++ (deprecado em C++)
}

static void on_peer_data(uint32_t msg_id, const uint8_t* data, size_t n, void*)
{
  switch (msg_id) {
    case C6_MSG_FAREJO:
      juntar_lote(data, n);
      break;
    case C6_MSG_INFO:
      if (n >= C6_INFO_V1_TAM) {
        // Um C6 mais velho manda o INFO curto (sem o diagnostics): o resto stays zero.
        memset((void*)&s_info, 0, sizeof(c6_info_t));
        memcpy((void*)&s_info, data, n < sizeof(c6_info_t) ? n : sizeof(c6_info_t));
        s_proto = s_info.proto;
        s_caps = s_info.caps;
        s_tem_info = true;
        if (s_info_sem) { xSemaphoreGive(s_info_sem); }
      }
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Renovacao - vaga of pool, so enquanto enabled.
// ---------------------------------------------------------------------------
static void vTarefaRenova(void*)
{
  while (!stop_requested(&s_parada)) {
    c6_farejo_cfg_t cfg;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    cfg = s_cfg;
    xSemaphoreGive(s_mtx);
    cfg.liga = 1;
    eh_host_peer_data_send(C6_MSG_HOST_FAREJO, (const uint8_t*)&cfg, sizeof(cfg));

    // Troca em pedacos para responder rapido a one disable.
    for (int i = 0; i < RENOVA_MS / 100 && !stop_requested(&s_parada); ++i) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  // Desliga no C6 e released a tabela.
  c6_farejo_cfg_t off = { 0 };
  eh_host_peer_data_send(C6_MSG_HOST_FAREJO, (const uint8_t*)&off, sizeof(off));

  xSemaphoreTake(s_mtx, portMAX_DELAY);
  if (s_tab) { heap_caps_free(s_tab); s_tab = nullptr; }
  s_n = 0;
  xSemaphoreGive(s_mtx);

  tab5_wifi_keep_awake(TAB5_AWAKE_SNIFFING, false);
  ESP_LOGI(TAG, "farejo desligado");
  stop_confirm(&s_parada);   // a ULTIMA coisa: daqui em diante one enable pode create outra
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
static void registrar(void)
{
  if (s_registrado) { return; }
  eh_host_peer_data_register(C6_MSG_FAREJO, on_peer_data, nullptr);
  eh_host_peer_data_register(C6_MSG_INFO,   on_peer_data, nullptr);
  s_registrado = true;
}

bool tab5_farejo_c6_suporta(void)
{
  return (s_caps & C6_CAP_FAREJO) != 0;
}

uint8_t tab5_farejo_caps_c6(void) { return s_caps; }

void tab5_farejo_ouvinte(tab5_farejo_ouvinte_t cb) { tab5_listener_set(&s_ouvinte, cb); }

bool tab5_farejo_enable(const c6_farejo_cfg_t* cfg)
{
  if (!cfg) { return false; }
  start_travas();
  if (!tab5_wifi_c6_is_ours() || !tab5_wifi_c6_link()) { return false; }
  registrar();

  tab5_mutex_lock(&s_trava_cmd);
  const bool vivo = stop_running(&s_parada);
  if (vivo && stop_requested(&s_parada) && !stop_request(&s_parada, 1500)) {
    // Saindo e nao exited: enable now dividiria a tabela com quem a libera.
    tab5_mutex_destravar(&s_trava_cmd);
    ESP_LOGE(TAG, "a tarefa anterior do farejo ainda nao saiu - nao liguei");
    return false;
  }
  const bool of_pe = vivo && stop_running(&s_parada);   // nao estava saindo

  xSemaphoreTake(s_mtx, portMAX_DELAY);
  s_cfg = *cfg;
  s_cfg.liga = 1;
  if (!s_tab) {
    s_tab = (farejo_item_t*)heap_caps_calloc(FAREJO_MAX, sizeof(farejo_item_t),
                                             MALLOC_CAP_SPIRAM);
    s_n = 0;
  }
  const bool sem_tab = (s_tab == nullptr);
  xSemaphoreGive(s_mtx);
  if (sem_tab) { tab5_mutex_destravar(&s_trava_cmd); ESP_LOGE(TAG, "sem PSRAM para a tabela"); return false; }

  // Receptor sempre enabled: o promiscuo perde o que chega durante o cochilo.
  tab5_wifi_keep_awake(TAB5_AWAKE_SNIFFING, true);

  s_quer = true;
  if (!of_pe) {
    if (!stop_prepare(&s_parada)
     || !pool_create(&s_id, vTarefaRenova, nullptr, "farejo", 3072, 3,
                    OWNER_SERVICE, "Farejo")) {
      stop_abandon(&s_parada);
      s_quer = false;
      tab5_wifi_keep_awake(TAB5_AWAKE_SNIFFING, false);
      tab5_mutex_destravar(&s_trava_cmd);
      ESP_LOGE(TAG, "sem vaga para a tarefa do farejo");
      return false;
    }
  } else {
    // Ja running: o next ciclo of renovacao manda a config nova.
    pool_wake(&s_id);
  }
  tab5_mutex_destravar(&s_trava_cmd);
  ESP_LOGI(TAG, "farejo ligado: mascara 0x%04x, lote %u ms",
           (unsigned)cfg->canais, (unsigned)cfg->lote_ms);
  return true;
}

void tab5_farejo_disable(void)
{
  tab5_mutex_lock(&s_trava_cmd);
  s_quer = false;
  // Pede a saida e nao espera (quem desliga e a screen): a tarefa exit in iteration
  // seguinte, released a tabela e confirma.
  if (stop_running(&s_parada)) { s_parada.request = true; pool_wake(&s_id); }
  tab5_mutex_destravar(&s_trava_cmd);
}

bool tab5_farejo_active(void) { return s_quer; }

int tab5_farejo_n(void)
{
  if (!s_mtx) { return 0; }
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  int n = s_n;
  xSemaphoreGive(s_mtx);
  return n;
}

bool tab5_farejo_item(int i, farejo_item_t* out)
{
  if (!s_mtx || !out) { return false; }
  bool ok = false;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  if (s_tab && i >= 0 && i < s_n) { *out = s_tab[i]; ok = true; }
  xSemaphoreGive(s_mtx);
  return ok;
}

bool tab5_farejo_achar(const uint8_t mac[6], farejo_item_t* out)
{
  if (!s_mtx || !out) { return false; }
  bool ok = false;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  if (s_tab) { int j = achar_mac(mac); if (j >= 0) { *out = s_tab[j]; ok = true; } }
  xSemaphoreGive(s_mtx);
  return ok;
}

uint32_t tab5_farejo_lotes(void)    { return s_lotes; }
uint32_t tab5_farejo_perdidos(void) { return s_perdidos; }

void tab5_farejo_limpar(void)
{
  if (!s_mtx) { return; }
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  if (s_tab) { memset(s_tab, 0, FAREJO_MAX * sizeof(farejo_item_t)); }
  s_n = 0;
  xSemaphoreGive(s_mtx);
  s_perdidos = 0;
}

bool tab5_farejo_info_c6(c6_info_t* out, uint32_t espera_ms)
{
  if (!out) { return false; }
  start_travas();
  if (!tab5_wifi_c6_is_ours() || !tab5_wifi_c6_link()) { return false; }
  registrar();
  s_tem_info = false;
  if (s_info_sem) { xSemaphoreTake(s_info_sem, 0); }   // esvazia
  const uint8_t chave = C6_AJ_INFO;
  uint8_t request[2] = { C6_AJ_INFO, 0 };
  (void)chave;
  eh_host_peer_data_send(C6_MSG_HOST_AJUSTE, request, 1);
  if (s_info_sem && xSemaphoreTake(s_info_sem, pdMS_TO_TICKS(espera_ms)) == pdTRUE) {
    memcpy(out, (const void*)&s_info, sizeof(c6_info_t));
    return true;
  }
  return false;
}

void tab5_farejo_anotar_pronto(const uint8_t* data, uint16_t n)
{
  // PRONTO v2: 6B MAC + 1B canal + 1B proto + 1B caps. Antigo: so 7 bytes.
  if (n >= 9) { s_proto = data[7]; s_caps = data[8]; }
  else        { s_proto = 1; s_caps = C6_CAP_ESPNOW; }
}
