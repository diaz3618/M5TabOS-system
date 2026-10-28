#include "tab5_ble.hpp"
#include "tab5_listener.hpp"
#include "tab5_signal.hpp"
#include "tab5_wifi.hpp"
#include "tab5_owners.hpp"
#include "task_pool.h"   // ligar/desligar numa vaga, sem tarefa propria

#include "esp_hosted_bt_host_stack.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "tab5_time.hpp"   // tab5_agora_ms
#include "tab5_monitor.hpp"   // tab5_mem_marco
#include <cstdio>
#include <cstring>

static const char* TAG = "ble";

extern "C" void ble_store_config_init(void);

// Quanto esperar o C6 subir o enlace (primeira vez no boot: o WiFi pode estar
// desligado e a stack do esp_hosted nasce agora) e a NimBLE sincronizar com o
// controlador dele.
#define PRAZO_PILHA_MS  20000
#define PRAZO_SYNC_MS    8000


// LIGAR/DESLIGAR SEM TAREFA PROPRIA. Havia uma tarefa de 4 KB (RAM interna)
// dormindo para sempre so para atender estes dois pedidos raros. Agora o
// pedido vai para uma vaga do pool de tarefas (kernel/task_pool.h), que
// existe so enquanto a subida/descida dura. A ORDEM continua garantida: so
// uma execucao por vez (s_rodando), e cada pedido ganha um numero - a
// execucao so termina quando atendeu o ultimo numero pedido, e quem chega
// enquanto ela roda nao se perde (ver disparar).
static volatile bool     s_quer      = false;   // o ultimo pedido: ligado?
static volatile uint32_t s_pedido    = 0;       // numero do ultimo pedido
static volatile uint32_t s_atendido  = 0;       // numero do ultimo atendido
static volatile bool     s_rodando   = false;   // ha uma execucao na vaga
static bool              s_iniciado  = false;
static SemaphoreHandle_t      s_trava = nullptr;       // a lista de vistos
static SemaphoreHandle_t      s_sync = nullptr;        // a NimBLE sincronizou
// A PILHA EM USO. Varrer/cancelar (de qualquer tarefa) e descer (tarefa do
// BLE) passam por aqui: sem isto, um ble_gap_disc da tela podia cair no meio
// do nimble_port_deinit - mutex da NimBLE ja destruido, e o aparelho reinicia.
static SemaphoreHandle_t      s_pilha = nullptr;
static volatile bool          s_ressincronizar = false; // a pilha reiniciou sozinha
static volatile tab5_ble_estado_t s_estado = TAB5_BLE_DESLIGADO;
static volatile bool          s_varrendo = false;
static volatile uint32_t      s_versao = 1;
static bool                   s_pilha_nimble = false;  // nimble_port_init feito
static ble_uuid_any_t         s_uuid_mesh;

static tab5_ble_visto_t s_vistos[TAB5_BLE_VISTOS];
static int              s_n_vistos = 0;
static int8_t           s_rssi_publicado[TAB5_BLE_VISTOS];   // o que a tela ja mostrou

static void mudou(void) { s_versao = s_versao + 1; }

// ---------------------------------------------------------------------------
// STACK
// ---------------------------------------------------------------------------
static void ao_sincronizar(void)
{
  // Endereco proprio: o publico do C6. Sem este passo o primeiro ble_gap_*
  // devolve BLE_HS_ENOADDR.
  ble_hs_util_ensure_addr(0);
  xSemaphoreGive(s_sync);
  // Depois de um reinicio sozinho da pilha (ao_reiniciar) ninguem mais volta
  // o estado para PRONTO: o subir() ja acabou ha muito tempo.
  if (s_ressincronizar && s_estado == TAB5_BLE_SUBINDO) {
    s_ressincronizar = false;
    s_estado = TAB5_BLE_PRONTO;
    mudou();
  }
}

static void ao_reiniciar(int motivo)
{
  ESP_LOGW(TAG, "a pilha reiniciou (motivo %d)", motivo);
  s_varrendo = false;
  // Descendo ou ainda subindo: quem esta atendendo o pedido (execucao) decide o estado.
  if (s_estado == TAB5_BLE_PRONTO) { s_ressincronizar = true; s_estado = TAB5_BLE_SUBINDO; }
  mudou();
}

static void tarefa_pilha(void*)
{
  nimble_port_run();            // so volta no nimble_port_stop()
  // A tarefa vai morrer: o registro de donos nao pode ficar com o handle
  // (o FreeRTOS reaproveita, e outra tarefa herdaria o dono "Bluetooth").
  tab5_dono_esquecer(xTaskGetCurrentTaskHandle());
  nimble_port_freertos_deinit();
}

static bool subir(void)
{
  // O C6 precisa estar no ar. Se o WiFi esta desligado a stack do esp_hosted
  // ainda nem existe: quem pediu o ligar ja chamou tab5_wifi_garantir_pilha()
  // (tarefa de interface, por causa do I2C); aqui so esperamos ela ficar
  // pronta.
  for (uint32_t t = 0; !tab5_wifi_pilha_de_pe() && t < PRAZO_PILHA_MS; t += 100) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (!tab5_wifi_pilha_de_pe()) {
    ESP_LOGE(TAG, "o C6 nao subiu em %d s - sem Bluetooth", PRAZO_PILHA_MS / 1000);
    return false;
  }

  if (!s_pilha_nimble) {
    esp_hosted_bt_host_stack_cfg_t cfg = ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT();
    esp_err_t err = esp_hosted_bt_host_stack_setup(&cfg);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "o C6 nao entregou o controlador BLE (%s) - firmware dele sem BT?",
               esp_err_to_name(err));
      return false;
    }
    tab5_mem_marco("BLE: controlador do C6");
    err = nimble_port_init();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
      esp_hosted_bt_host_stack_teardown();
      return false;
    }

    ble_hs_cfg.sync_cb  = ao_sincronizar;
    ble_hs_cfg.reset_cb = ao_reiniciar;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // SEGURANCA: o Meshtastic exige pareamento com PIN. O Tab5 e o lado que
    // DIGITA o PIN (o no mostra ou tem um fixo) - "so teclado". Vinculo
    // guardado (NVS): o PIN so e pedido na primeira vez.
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();

    s_pilha_nimble = true;
    xSemaphoreTake(s_sync, 0);
    nimble_port_freertos_init(tarefa_pilha);
    TaskHandle_t h = xTaskGetHandle("nimble_host");
    if (h) { tab5_dono_registrar(h, DONO_SERVICO, "Bluetooth"); }
    tab5_mem_marco("NimBLE (pilha + tarefa)");
  }

  if (xSemaphoreTake(s_sync, pdMS_TO_TICKS(PRAZO_SYNC_MS)) != pdTRUE && !ble_hs_synced()) {
    ESP_LOGE(TAG, "a pilha nao sincronizou com o controlador do C6");
    return false;
  }
  return true;
}

static bool descer(void)
{
  if (!s_pilha_nimble) { return true; }
  xSemaphoreTake(s_pilha, portMAX_DELAY);
  if (s_varrendo) { ble_gap_disc_cancel(); s_varrendo = false; }
  bool ok = false;
  // O nimble_port_stop() ja derruba as conexoes abertas (ble_hs_stop) e
  // espera a tarefa da pilha sair do laco.
  if (nimble_port_stop() == 0) {
    nimble_port_deinit();
    esp_hosted_bt_host_stack_teardown();
    s_pilha_nimble = false;
    ok = true;
  } else {
    ESP_LOGW(TAG, "a pilha nao parou - fica de pe");
  }
  xSemaphoreGive(s_pilha);
  return ok;
}

// Um pedido: o mesmo corpo da tarefa antiga, para ligar ou desligar.
static void aplicar(bool ligar)
{
  if (ligar) {
    if (s_estado == TAB5_BLE_PRONTO) { return; }
    s_estado = TAB5_BLE_SUBINDO; mudou();
    const bool ok = subir();
    s_estado = ok ? TAB5_BLE_PRONTO : TAB5_BLE_FALHOU; mudou();
    if (ok) { ESP_LOGI(TAG, "Bluetooth no ar (pilha NimBLE no P4, radio do C6)"); }
  } else {
    if (s_estado == TAB5_BLE_DESLIGADO) { return; }
    if (descer()) {
      s_estado = TAB5_BLE_DESLIGADO; mudou();
      ESP_LOGI(TAG, "Bluetooth desligado");
    } else {
      s_estado = TAB5_BLE_PRONTO; mudou();
    }
  }
}

static void disparar(void);

static void execucao(void*)
{
  uint32_t n;
  do {
    n = s_pedido;
    aplicar(s_quer);
    s_atendido = n;
  } while (s_atendido != s_pedido);   // chegou outro no meio: atende tambem
  s_rodando = false;
  // Um pedido que chegou entre o while e a linha de cima achou s_rodando
  // ainda de pe e nao disparou: confere de novo.
  if (s_atendido != s_pedido) { disparar(); }
}

static void disparar(void)
{
  bool livre = false;
  if (!__atomic_compare_exchange_n(&s_rodando, &livre, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    return;   // ja ha uma execucao: ela atende este pedido tambem
  }
  if (!pool_criar(nullptr, execucao, nullptr, "ble", 4096, 3, DONO_SERVICO, "Bluetooth")) {
    s_rodando = false;
    ESP_LOGE(TAG, "sem vaga para ligar/desligar o Bluetooth");
  }
}

static void pedir(bool ligar)
{
  s_quer = ligar;
  __atomic_add_fetch(&s_pedido, 1, __ATOMIC_ACQ_REL);
  disparar();
}

// ---------------------------------------------------------------------------
// VARREDURA
// ---------------------------------------------------------------------------
// RASTREADORES E BEACONS - reconhecidos pelos campos publicos do anuncio.
//   Apple (empresa 0x004C): tipo 0x12 = rede Find My (AirTag, e iPhone/Mac
//     offline); tipo 0x02 0x15 = iBeacon, com o RSSI a 1 m no ultimo byte.
//   Tile: servico 0xFEED/0xFEEC.  SmartTag (Samsung): servico 0xFD5A.
//   Google 0xFEAA: quadro 0x40/0x41 = rede Encontre Meu Dispositivo; os
//     outros quadros sao Eddystone (o 0x00 e o 0x10 trazem o RSSI a 0 m).
static bool uuid16_anunciado(const struct ble_hs_adv_fields& f, uint16_t u)
{
  for (int k = 0; k < f.num_uuids16; ++k) { if (f.uuids16[k].value == u) { return true; } }
  return f.svc_data_uuid16 && f.svc_data_uuid16_len >= 2
      && (uint16_t)(f.svc_data_uuid16[0] | (f.svc_data_uuid16[1] << 8)) == u;
}

static void classificar(tab5_ble_visto_t* v, const struct ble_hs_adv_fields& f)
{
  const uint8_t* m = f.mfg_data;
  const int mn = f.mfg_data ? f.mfg_data_len : 0;
  auto marcar = [v](uint8_t classe, const char* marca) {
    v->classe = classe;
    snprintf(v->marca, sizeof(v->marca), "%s", marca);
  };
  if (mn >= 3 && m[0] == 0x4C && m[1] == 0x00) {
    if (m[2] == 0x12) { marcar(TAB5_BLE_RASTREADOR, "Find My"); }
    else if (mn >= 25 && m[2] == 0x02 && m[3] == 0x15) { marcar(TAB5_BLE_BEACON, "iBeacon"); v->p1m = (int8_t)m[24]; }
  }
  if (uuid16_anunciado(f, 0xFEED) || uuid16_anunciado(f, 0xFEEC)) { marcar(TAB5_BLE_RASTREADOR, "Tile"); }
  if (uuid16_anunciado(f, 0xFD5A)) { marcar(TAB5_BLE_RASTREADOR, "SmartTag"); }
  if (f.svc_data_uuid16 && f.svc_data_uuid16_len >= 3
      && f.svc_data_uuid16[0] == 0xAA && f.svc_data_uuid16[1] == 0xFE) {
    const uint8_t quadro = f.svc_data_uuid16[2];
    if (quadro == 0x40 || quadro == 0x41) { marcar(TAB5_BLE_RASTREADOR, "Google"); }
    else {
      marcar(TAB5_BLE_BEACON, "Eddystone");
      if ((quadro == 0x00 || quadro == 0x10) && f.svc_data_uuid16_len >= 4) { v->p1m = (int8_t)(f.svc_data_uuid16[3] - TAB5_SINAL_PERDA_1M); }
    }
  }
  if (v->p1m == 0 && f.tx_pwr_lvl_is_present) { v->p1m = (int8_t)(f.tx_pwr_lvl - TAB5_SINAL_PERDA_1M); }
}

static void anotar(const ble_addr_t* a, int8_t rssi, const uint8_t* dados, uint8_t n)
{
  struct ble_hs_adv_fields f;
  const bool campos = (ble_hs_adv_parse_fields(&f, dados, n) == 0);

  xSemaphoreTake(s_trava, portMAX_DELAY);
  int i = 0;
  for (; i < s_n_vistos; ++i) {
    if (memcmp(s_vistos[i].addr, a->val, 6) == 0) { break; }
  }
  bool novo = false;
  if (i == s_n_vistos) {
    if (s_n_vistos < TAB5_BLE_VISTOS) {
      ++s_n_vistos;
    } else {
      // Cheia: sai o visto ha mais tempo.
      i = 0;
      for (int k = 1; k < TAB5_BLE_VISTOS; ++k) {
        if (s_vistos[k].visto_ms < s_vistos[i].visto_ms) { i = k; }
      }
    }
    memset(&s_vistos[i], 0, sizeof(s_vistos[i]));
    memcpy(s_vistos[i].addr, a->val, 6);
    novo = true;
  }
  tab5_ble_visto_t* v = &s_vistos[i];
  v->tipo = a->type;
  v->rssi = novo ? rssi : (int8_t)((v->rssi * 3 + rssi) / 4);
  // A distancia na tela acompanha quem anda com o aparelho procurando uma
  // etiqueta: 4 dB de diferenca (~40% na distancia) ja e novidade.
  if (novo || v->rssi - s_rssi_publicado[i] >= 4 || s_rssi_publicado[i] - v->rssi >= 4) {
    s_rssi_publicado[i] = v->rssi;
    novo = true;
  }
  v->visto_ms = tab5_agora_ms();
  if (campos) {
    // O nome vem na resposta de varredura, o servico no anuncio: cada relato
    // traz uma parte, e o que ja se sabe nao e apagado por um relato sem ela.
    if (f.name && f.name_len) {
      const size_t k = f.name_len < sizeof(v->nome) - 1 ? f.name_len : sizeof(v->nome) - 1;
      if (k != strlen(v->nome) || memcmp(v->nome, f.name, k) != 0) { novo = true; }
      memcpy(v->nome, f.name, k);
      v->nome[k] = 0;
    }
    // Pelo nome tambem: nem todo firmware poe o UUID do servico no anuncio
    // (cabe no pacote de 31 bytes junto do nome so as vezes), e o nome padrao
    // do Meshtastic comeca por "Meshtastic".
    if (!v->meshtastic && strncmp(v->nome, "Meshtastic", 10) == 0) { v->meshtastic = true; novo = true; }
    for (int k = 0; k < f.num_uuids128; ++k) {
      if (ble_uuid_cmp(&f.uuids128[k].u, &s_uuid_mesh.u) == 0) {
        if (!v->meshtastic) { novo = true; }
        v->meshtastic = true;
      }
    }
    const uint8_t antes = v->classe;
    classificar(v, f);
    if (v->meshtastic) { v->classe = TAB5_BLE_MESHTASTIC; }
    if (v->classe != antes) { novo = true; }
  }
  xSemaphoreGive(s_trava);
  if (novo) { mudou(); }
}

static tab5_ouvinte_t<tab5_ble_ouvinte_t> s_ouvinte;   // remover espera a chamada em voo

static int ao_evento_gap(struct ble_gap_event* ev, void*)
{
  switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
      anotar(&ev->disc.addr, ev->disc.rssi, ev->disc.data, ev->disc.length_data);
      // Fora da trava da lista. Quem tira o ouvinte espera esta chamada
      // terminar (tab5_listener.hpp) antes de destruir o que ele usa.
      tab5_ouvinte_chamar(&s_ouvinte, (const uint8_t*)ev->disc.addr.val, ev->disc.rssi);
      break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
      s_varrendo = false;
      mudou();
      break;
    default: break;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
void tab5_ble_iniciar(void)
{
  if (s_iniciado) { return; }
  s_iniciado = true;
  s_trava = xSemaphoreCreateMutex();
  s_sync  = xSemaphoreCreateBinary();
  s_pilha = xSemaphoreCreateMutex();
  ble_uuid_from_str(&s_uuid_mesh, TAB5_BLE_UUID_MESHTASTIC);
  // Sem tarefa: ligar/desligar vao para uma vaga do pool (ver disparar).
}

void tab5_ble_ligar(void)
{
  if (!s_iniciado) { return; }
  tab5_wifi_garantir_pilha();   // I2C do trilho do radio: por isso "da tarefa de interface"
  pedir(true);
}

void tab5_ble_desligar(void)
{
  if (!s_iniciado) { return; }
  // Ja, e nao quando a execucao pegar o pedido: daqui em diante ninguem mais
  // ve "pronto" e comeca uma varredura ou conexao nova.
  if (s_estado != TAB5_BLE_DESLIGADO) { s_estado = TAB5_BLE_DESLIGANDO; mudou(); }
  pedir(false);
}

tab5_ble_estado_t tab5_ble_estado(void) { return s_estado; }
bool tab5_ble_pronto(void) { return s_estado == TAB5_BLE_PRONTO; }

const char* tab5_ble_descricao(void)
{
  switch (s_estado) {
    case TAB5_BLE_DESLIGADO: return "desligado";
    case TAB5_BLE_SUBINDO:   return "ligando";
    case TAB5_BLE_PRONTO:    return s_varrendo ? "procurando aparelhos" : "ligado";
    case TAB5_BLE_FALHOU:    return "falhou (o C6 não deu o Bluetooth)";
    case TAB5_BLE_DESLIGANDO: return "desligando";
  }
  return "?";
}

bool tab5_ble_varrer(uint32_t ms)
{
  if (!s_pilha || !tab5_ble_pronto() || s_varrendo) { return false; }
  // Sem esperar: se a pilha esta descendo agora, a resposta e "nao deu".
  if (xSemaphoreTake(s_pilha, 0) != pdTRUE) { return false; }
  if (!tab5_ble_pronto() || !s_pilha_nimble) { xSemaphoreGive(s_pilha); return false; }
  uint8_t proprio;
  if (ble_hs_id_infer_auto(0, &proprio) != 0) { xSemaphoreGive(s_pilha); return false; }
  struct ble_gap_disc_params p;
  memset(&p, 0, sizeof(p));
  p.passive = 0;            // ativa: o nome vem na resposta de varredura
  p.filter_duplicates = 0;  // o nome e o servico chegam em relatos separados
  const int rc = ble_gap_disc(proprio, (int32_t)ms, &p, ao_evento_gap, nullptr);
  if (rc == 0) { s_varrendo = true; }
  xSemaphoreGive(s_pilha);
  if (rc != 0) {
    ESP_LOGW(TAG, "varredura recusada (%d)", rc);   // BLE_HS_EBUSY: conectando
    return false;
  }
  mudou();
  return true;
}

bool tab5_ble_varrendo(void) { return s_varrendo; }

void tab5_ble_ouvinte(tab5_ble_ouvinte_t f) { tab5_ouvinte_definir(&s_ouvinte, f); }

void tab5_ble_varredura_parar(void)
{
  if (!s_varrendo || !s_pilha) { return; }
  // Se a pilha esta descendo, o descer() ja cancela - nao espere por ela.
  if (xSemaphoreTake(s_pilha, 0) != pdTRUE) { return; }
  if (s_pilha_nimble && s_varrendo) { ble_gap_disc_cancel(); }
  s_varrendo = false;
  xSemaphoreGive(s_pilha);
  mudou();
}

int tab5_ble_n_vistos(void) { return s_n_vistos; }

bool tab5_ble_visto(int i, tab5_ble_visto_t* out)
{
  if (!s_trava || !out) { return false; }
  xSemaphoreTake(s_trava, portMAX_DELAY);
  const bool ok = (i >= 0 && i < s_n_vistos);
  if (ok) { *out = s_vistos[i]; }
  xSemaphoreGive(s_trava);
  return ok;
}

uint32_t tab5_ble_versao(void) { return s_versao; }

void tab5_ble_endereco_txt(const uint8_t a[6], char out[18])
{
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

bool tab5_ble_endereco_ler(const char* txt, uint8_t a[6])
{
  unsigned b[6];
  if (!txt || sscanf(txt, "%2x:%2x:%2x:%2x:%2x:%2x", &b[5], &b[4], &b[3], &b[2], &b[1], &b[0]) != 6) {
    return false;
  }
  for (int k = 0; k < 6; ++k) { a[k] = (uint8_t)b[k]; }
  return true;
}
