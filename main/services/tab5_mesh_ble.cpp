#include "tab5_mesh_ble.hpp"
#include "tab5_time.hpp"   // tab5_agora_ms
#include "tab5_ble.hpp"
#include "tab5_owners.hpp"
#include "task_pool.h"
#include "service_stop.hpp"      // parada cooperativa da tarefa (kernel)
#include "settings.h"
#include "notification.h"

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_store.h"

#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/message_buffer.h"
#include <cstdio>
#include <cstring>

static const char* TAG = "mesh_ble";

// UUIDs do servico do Meshtastic (docs: meshtastic.org, "Bluetooth API").
#define UUID_TORADIO   "f75c76d2-129e-4dad-a1dd-7866124401e7"
#define UUID_FROMRADIO "2c55e69e-4993-11ed-b878-0242ac120002"
#define UUID_FROMNUM   "ed9da18c-a800-4f66-a670-aa7547e34453"

#define CORPO_MAX       512    // MAX_TO_FROM_RADIO_SIZE do firmware
#define PRAZO_OP_MS   10000    // cada operacao GATT
#define PRAZO_PAR_MS  30000    // pareamento (o no pode estar mostrando o PIN)
#define VOLTA_MS       5000    // leitura de seguranca, mesmo sem aviso do fromNum

typedef struct { uint16_t n; uint8_t d[CORPO_MAX]; } envio_t;

// ---- estado ----------------------------------------------------------------
static TaskHandle_t        s_tarefa = nullptr;
static QueueHandle_t       s_tx = nullptr;
static MessageBufferHandle_t s_rx = nullptr;
static SemaphoreHandle_t   s_op = nullptr;        // fim de uma operacao GATT/GAP
static volatile int        s_op_rc = 0;
static parada_t            s_parada = { };      // pedido de parada / saida confirmada da tarefa
static volatile bool       s_conectado = false, s_conectou_agora = false;
static volatile bool       s_ler = false;
static volatile uint16_t   s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool       s_pin_pedido = false;
// PIN NA HORA. O Meshtastic tem dois modos: PIN FIXO (123456 nos
// aparelhos sem tela, ou o que se configurou) e PIN ALEATORIO, que o no mostra
// na tela A CADA pareamento - um PIN salvo nunca acerta esse. Por isso: a
// primeira tentativa usa o PIN de Ajustes; se o no recusar, a proxima e
// INTERATIVA - o pedido fica esperando (o pareamento da ~30 s) e Ajustes >
// Meshtastic abre o teclado para digitar o que o no esta mostrando.
static volatile bool       s_pin_interativo = false;   // a proxima vez, pergunta
static volatile bool       s_pin_esperando = false;    // ha um pedido esperando o PIN agora
static volatile uint16_t   s_pin_conn = 0;
static int                 s_notif = -1;
static char                s_estado[48] = "parado";
static char                s_nome[24] = "", s_end[18] = "";

static ble_uuid_any_t s_u_svc, s_u_to, s_u_from, s_u_num;
static uint16_t s_svc_ini, s_svc_fim, s_h_to, s_h_from, s_h_num, s_h_cccd;

// O envio sendo escrito (so a tarefa do enlace usa) - PSRAM, fora da stack.
EXT_RAM_BSS_ATTR static envio_t s_envio;

// Leitura em andamento (callback da NimBLE -> tarefa)
static uint8_t  s_lido[CORPO_MAX];
static uint16_t s_lido_n;

// Cada mudanca de estado tambem vai para o log: uma tentativa de conexao que
// falha deixa o rastro inteiro (procurando -> conectando -> pareando -> ...).
static void estado(const char* fmt, const char* arg = nullptr)
{
  char novo[sizeof(s_estado)];
  snprintf(novo, sizeof(novo), fmt, arg ? arg : "");
  if (strcmp(novo, s_estado) != 0) { ESP_LOGI(TAG, "%s", novo); }
  snprintf(s_estado, sizeof(s_estado), "%s", novo);
}

static int esperar_op(uint32_t ms)
{
  if (parada_pedida(&s_parada)) { return BLE_HS_ENOTCONN; }   // pediram para parar: nada de operacao nova
  if (xSemaphoreTake(s_op, pdMS_TO_TICKS(ms)) != pdTRUE) { return BLE_HS_ETIMEOUT; }
  return s_op_rc;
}
static void fim_op(int rc) { s_op_rc = rc; xSemaphoreGive(s_op); }

// ---------------------------------------------------------------------------
// EVENTOS DA CONEXAO (tarefa da NimBLE - curto, so anota e acorda)
// ---------------------------------------------------------------------------
static int ao_gap(struct ble_gap_event* ev, void*)
{
  switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (ev->connect.status == 0) { s_conn = ev->connect.conn_handle; }
      fim_op(ev->connect.status);
      break;

    case BLE_GAP_EVENT_DISCONNECT:
      ESP_LOGI(TAG, "desconectado (motivo 0x%x)", ev->disconnect.reason);
      s_conn = BLE_HS_CONN_HANDLE_NONE;
      s_conectado = false;
      fim_op(BLE_HS_ENOTCONN);   // destrava quem esperava uma operacao
      if (s_tarefa) { xTaskNotifyGive(s_tarefa); }
      break;

    case BLE_GAP_EVENT_ENC_CHANGE:
      fim_op(ev->enc_change.status);
      break;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
      // O PIN. O no diz o que quer: DIGITAR (o comum - ele mostra ou tem um
      // fixo, 123456 nos sem tela) ou CONFERIR um numero (aceitamos).
      struct ble_sm_io io;
      memset(&io, 0, sizeof(io));
      io.action = ev->passkey.params.action;
      if (io.action == BLE_SM_IOACT_INPUT) {
        s_pin_pedido = true;
        if (s_pin_interativo || ajustes_mesh_pin_na_tela()) {
          s_pin_interativo = true;
          // Espera quem digitar (mesh_ble_digitar_pin): nada e injetado aqui.
          s_pin_conn = ev->passkey.conn_handle;
          s_pin_esperando = true;
          estado("digite o PIN que o nó mostra");
          if (s_notif >= 0) { notif_publicar(s_notif, 1, NOTIF_ATENCAO, "PIN do Meshtastic"); }
          ESP_LOGI(TAG, "o no pediu o PIN - esperando o usuario (Ajustes > Meshtastic)");
          break;
        }
        io.passkey = ajustes_mesh_pin();
        ESP_LOGI(TAG, "o no pediu o PIN - usando o de Ajustes");
      } else if (io.action == BLE_SM_IOACT_NUMCMP) {
        io.numcmp_accept = 1;
      }
      ble_sm_inject_io(ev->passkey.conn_handle, &io);
      break;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      // O no esqueceu o vinculo (reset) e quer parear de novo: apagamos o
      // nosso e deixamos repetir - senao ficaria recusando para sempre.
      struct ble_gap_conn_desc d;
      if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) {
        ble_store_util_delete_peer(&d.peer_id_addr);
      }
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_NOTIFY_RX:
      if (ev->notify_rx.attr_handle == s_h_num) {
        s_ler = true;
        if (s_tarefa) { xTaskNotifyGive(s_tarefa); }
      }
      break;

    default: break;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// GATT SINCRONO - uma operacao por vez, sempre da tarefa do enlace
// ---------------------------------------------------------------------------
static int cb_mtu(uint16_t, const struct ble_gatt_error* e, uint16_t mtu, void*)
{
  if (e->status == 0) { ESP_LOGI(TAG, "MTU %u", (unsigned)mtu); }
  fim_op(e->status);
  return 0;
}

static int cb_svc(uint16_t, const struct ble_gatt_error* e, const struct ble_gatt_svc* s, void*)
{
  if (e->status == 0) { s_svc_ini = s->start_handle; s_svc_fim = s->end_handle; return 0; }
  fim_op(e->status == BLE_HS_EDONE ? 0 : e->status);
  return 0;
}

static int cb_chr(uint16_t, const struct ble_gatt_error* e, const struct ble_gatt_chr* c, void*)
{
  if (e->status == 0) {
    if      (ble_uuid_cmp(&c->uuid.u, &s_u_to.u)   == 0) { s_h_to   = c->val_handle; }
    else if (ble_uuid_cmp(&c->uuid.u, &s_u_from.u) == 0) { s_h_from = c->val_handle; }
    else if (ble_uuid_cmp(&c->uuid.u, &s_u_num.u)  == 0) { s_h_num  = c->val_handle; }
    return 0;
  }
  fim_op(e->status == BLE_HS_EDONE ? 0 : e->status);
  return 0;
}

static int cb_dsc(uint16_t, const struct ble_gatt_error* e, uint16_t chr_val,
                  const struct ble_gatt_dsc* d, void*)
{
  if (e->status == 0) {
    if (chr_val == s_h_num && !s_h_cccd
     && ble_uuid_u16(&d->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
      s_h_cccd = d->handle;
    }
    return 0;
  }
  fim_op(e->status == BLE_HS_EDONE ? 0 : e->status);
  return 0;
}

static int cb_ler(uint16_t, const struct ble_gatt_error* e, struct ble_gatt_attr* a, void*)
{
  if (e->status == 0 && a) {
    const uint16_t n = OS_MBUF_PKTLEN(a->om);
    if (a->offset + n <= CORPO_MAX) {
      os_mbuf_copydata(a->om, 0, n, s_lido + a->offset);
      s_lido_n = a->offset + n;
    }
    return 0;
  }
  fim_op(e->status == BLE_HS_EDONE ? 0 : e->status);
  return 0;
}

static int cb_escrever(uint16_t, const struct ble_gatt_error* e, struct ble_gatt_attr*, void*)
{
  fim_op(e->status);
  return 0;
}

static bool conexao_viva(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }

static int escrever(uint16_t h, const uint8_t* d, size_t n)
{
  if (!conexao_viva()) { return BLE_HS_ENOTCONN; }
  xSemaphoreTake(s_op, 0);
  const uint16_t mtu = ble_att_mtu(s_conn);
  int rc;
  if (n + 3 <= mtu) {
    rc = ble_gattc_write_flat(s_conn, h, d, (uint16_t)n, cb_escrever, nullptr);
  } else {
    struct os_mbuf* om = ble_hs_mbuf_from_flat(d, (uint16_t)n);
    rc = om ? ble_gattc_write_long(s_conn, h, 0, om, cb_escrever, nullptr) : BLE_HS_ENOMEM;
  }
  return rc ? rc : esperar_op(PRAZO_OP_MS);
}

// FromRadio: uma mensagem por leitura; leitura vazia = o no nao tem mais nada.
// Ate 64 por vez (a fila de envio nao pode esperar o dump inteiro). Devolve
// false quando parou no teto SEM esvaziar: o resto vem na volta seguinte, na
// hora - antes esperava o proximo aviso do fromNum (que so vem com pacote
// NOVO) ou os 5 s da leitura de seguranca, e um dump de muitos nos andava aos
// soluços.
static bool ler_ate_vazio(void)
{
  for (int k = 0; k < 64 && conexao_viva() && !parada_pedida(&s_parada); ++k) {
    xSemaphoreTake(s_op, 0);
    s_lido_n = 0;
    if (ble_gattc_read_long(s_conn, s_h_from, 0, cb_ler, nullptr) != 0) { return true; }
    if (esperar_op(PRAZO_OP_MS) != 0) { return true; }
    if (s_lido_n == 0) { return true; }
    if (xMessageBufferSend(s_rx, s_lido, s_lido_n, pdMS_TO_TICKS(500)) == 0) {
      ESP_LOGW(TAG, "o servico Mesh nao esta consumindo - mensagem de %u bytes perdida",
               (unsigned)s_lido_n);
    }
  }
  return !conexao_viva() || parada_pedida(&s_parada);
}

// ---------------------------------------------------------------------------
// ACHAR, CONECTAR, PAREAR, DESCOBRIR
// ---------------------------------------------------------------------------
static bool achar_no(ble_addr_t* alvo)
{
  char salvo[18];
  ajustes_mesh_ble_endereco(salvo);
  uint8_t quer[6];
  const bool tem_salvo = tab5_ble_endereco_ler(salvo, quer);

  estado(tem_salvo ? "procurando %s" : "procurando um no Meshtastic", salvo);
  // Se a tela ja esta varrendo, aproveitamos a varredura dela.
  // 8 s: com o Wi-Fi conectado o C6 divide o radio, e a busca ouve menos.
  if (!tab5_ble_varrendo() && !tab5_ble_varrer(8000)) { return false; }
  while (tab5_ble_varrendo() && !parada_pedida(&s_parada)) { vTaskDelay(pdMS_TO_TICKS(200)); }

  int melhor = -1;
  int8_t rssi = -128;
  tab5_ble_visto_t v;
  const uint32_t agora = tab5_agora_ms();
  for (int i = 0; i < tab5_ble_n_vistos(); ++i) {
    if (!tab5_ble_visto(i, &v) || agora - v.visto_ms > 15000) { continue; }
    if (tem_salvo ? (memcmp(v.addr, quer, 6) == 0) : (v.meshtastic && v.rssi > rssi)) {
      melhor = i; rssi = v.rssi;
      if (tem_salvo) { break; }
    }
  }
  if (melhor < 0) { return false; }
  tab5_ble_visto(melhor, &v);
  alvo->type = v.tipo;
  memcpy(alvo->val, v.addr, 6);
  snprintf(s_nome, sizeof(s_nome), "%s", v.nome[0] ? v.nome : "Meshtastic");
  tab5_ble_endereco_txt(v.addr, s_end);
  return true;
}

// 0 = conectado e pronto; <0 = tente de novo em breve; >0 = espere mais (PIN)
static int conectar(const ble_addr_t* alvo)
{
  uint8_t proprio;
  if (ble_hs_id_infer_auto(0, &proprio) != 0) { return -1; }

  estado("conectando a %s", s_nome);
  xSemaphoreTake(s_op, 0);
  int rc = ble_gap_connect(proprio, alvo, 10000, nullptr, ao_gap, nullptr);
  if (rc != 0) { ESP_LOGW(TAG, "ble_gap_connect: %d", rc); return -1; }
  rc = esperar_op(12000);
  if (rc != 0 || !conexao_viva()) {
    ESP_LOGW(TAG, "nao conectou (%d)", rc);
    ble_gap_conn_cancel();
    return -1;
  }

  // Os enderecos desta conexao: o que a stack acha que e o nosso (id), o que
  // foi de fato ao ar (ota) e o do no. A verificacao final do pareamento
  // seguro inclui os enderecos - se id e ota divergirem, e ali que ela quebra.
  {
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(s_conn, &d) == 0) {
      char a1[18], a2[18], a3[18];
      tab5_ble_endereco_txt(d.our_id_addr.val, a1);
      tab5_ble_endereco_txt(d.our_ota_addr.val, a2);
      tab5_ble_endereco_txt(d.peer_ota_addr.val, a3);
      ESP_LOGI(TAG, "enderecos: nosso id %s (tipo %u), no ar %s (tipo %u), no %s (tipo %u)",
               a1, d.our_id_addr.type, a2, d.our_ota_addr.type, a3, d.peer_ota_addr.type);
    }
  }

  xSemaphoreTake(s_op, 0);
  if (ble_gattc_exchange_mtu(s_conn, cb_mtu, nullptr) == 0) { esperar_op(PRAZO_OP_MS); }

  // Pareamento. Ja vinculado: so retoma a cifra, sem PIN. Sem vinculo: o no
  // pede o PIN (ver PASSKEY_ACTION).
  estado("pareando com %s", s_nome);
  s_pin_pedido = false;
  xSemaphoreTake(s_op, 0);
  rc = ble_gap_security_initiate(s_conn);
  if (rc == 0) { rc = esperar_op(PRAZO_PAR_MS); }
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    struct ble_gap_conn_desc d;
    const bool achou = conexao_viva() && ble_gap_conn_find(s_conn, &d) == 0;
    if (conexao_viva()) { ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM); }
    if (s_pin_esperando) {
      s_pin_esperando = false;
      if (s_notif >= 0) { notif_limpar(s_notif); }
    }
    // DHKEY CHECK FAILED: as 20 rodadas do PIN passaram (o PIN estava certo),
    // mas a verificacao final do pareamento seguro (LE Secure Connections)
    // nao bateu. O modo LEGADO com PIN nao tem essa etapa, e o Meshtastic o
    // aceita: troca para ele e pergunta o PIN de novo ja (o no mostra outro).
    if ((rc == BLE_HS_SM_PEER_ERR(BLE_SM_ERR_DHKEY) || rc == BLE_HS_SM_US_ERR(BLE_SM_ERR_DHKEY))
     && ble_hs_cfg.sm_sc) {
      ESP_LOGW(TAG, "verificacao final do pareamento seguro falhou (%d) - tentando o pareamento legado", rc);
      ble_hs_cfg.sm_sc = 0;
      s_pin_interativo = true;
      estado("tentando outro modo de pareamento - o no vai mostrar outro PIN");
      return -1;
    }
    // O PIN SALVO FOI RECUSADO: o no mostra um PIN novo a cada pareamento.
    // Lembra disso (NVS) e tenta de novo ja - agora perguntando o PIN.
    if (s_pin_pedido && !s_pin_interativo) {
      ESP_LOGW(TAG, "PIN salvo recusado (%d) - o no usa PIN na tela; pergunto a partir de agora", rc);
      ajustes_definir_mesh_pin_na_tela(true);
      ajustes_definir_mesh_pin(123456);   // o salvo estava errado: volta ao padrao
      s_pin_interativo = true;
      estado("o nó mostra o PIN na tela - tentando de novo");
      return -1;
    }
    if (s_pin_pedido) {
      // PIN errado (ou o prazo venceu). Nao repete sozinho: cada tentativa
      // incomoda o no e gera um PIN novo nele. A proxima pergunta o PIN na
      // hora, e so comeca quando o usuario pedir (mesh_ble_tentar_de_novo, que
      // o ENVIAR do PIN em Ajustes > Meshtastic chama).
      ESP_LOGW(TAG, "pareamento recusado (%d) - PIN errado ou prazo vencido", rc);
      estado("PIN recusado - toque no nó para tentar");
      s_pin_interativo = true;
      return 2;
    }
    // Sem PIN pedido e ainda assim falhou: o vinculo guardado nao vale mais
    // (o no foi resetado). Apaga e tenta do zero na proxima volta.
    if (achou) { ble_store_util_delete_peer(&d.peer_id_addr); }
    ESP_LOGW(TAG, "cifra recusada (%d) - vinculo antigo apagado", rc);
    return -1;
  }

  // Descoberta do servico e das tres caracteristicas.
  s_svc_ini = s_svc_fim = s_h_to = s_h_from = s_h_num = s_h_cccd = 0;
  xSemaphoreTake(s_op, 0);
  rc = ble_gattc_disc_svc_by_uuid(s_conn, &s_u_svc.u, cb_svc, nullptr);
  if (rc == 0) { rc = esperar_op(PRAZO_OP_MS); }
  if (rc == 0 && s_svc_ini) {
    xSemaphoreTake(s_op, 0);
    rc = ble_gattc_disc_all_chrs(s_conn, s_svc_ini, s_svc_fim, cb_chr, nullptr);
    if (rc == 0) { rc = esperar_op(PRAZO_OP_MS); }
  }
  if (rc == 0 && s_h_num) {
    xSemaphoreTake(s_op, 0);
    rc = ble_gattc_disc_all_dscs(s_conn, s_h_num, s_svc_fim, cb_dsc, nullptr);
    if (rc == 0) { rc = esperar_op(PRAZO_OP_MS); }
  }
  if (rc != 0 || !s_h_to || !s_h_from) {
    ESP_LOGW(TAG, "o aparelho nao tem o servico do Meshtastic completo (%d)", rc);
    if (conexao_viva()) { ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM); }
    return -1;
  }

  // Assina o fromNum. Sem ele ainda funciona (a volta de VOLTA_MS le assim
  // mesmo), so mais devagar.
  if (s_h_cccd) {
    const uint8_t liga[2] = { 0x01, 0x00 };
    if (escrever(s_h_cccd, liga, 2) != 0) { ESP_LOGW(TAG, "nao assinei o fromNum"); }
  }

  s_pin_interativo = false;
  if (s_notif >= 0) { notif_limpar(s_notif); }

  // Deste no em diante e este no: guarda para reconectar direto.
  ajustes_definir_mesh_ble_endereco(s_end);
  ESP_LOGI(TAG, "conectado a %s (%s)", s_nome, s_end);
  estado("conectado a %s", s_nome);
  s_conectado = true;
  s_conectou_agora = true;
  return 0;
}

// ---------------------------------------------------------------------------
// A TAREFA DO ENLACE
// ---------------------------------------------------------------------------
static void vTaskMeshBle(void*)
{
  uint32_t espera = 0;
  for (;;) {
    if (parada_pedida(&s_parada)) { break; }
    if (espera) {
      ulTaskNotifyTake(pdTRUE, espera == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(espera));
      espera = 0;
      continue;
    }

    if (!tab5_ble_pronto()) {
      estado(tab5_ble_estado() == TAB5_BLE_FALHOU ? "Bluetooth falhou" : "esperando o Bluetooth");
      espera = 1000;
      continue;
    }

    ble_addr_t alvo;
    if (!achar_no(&alvo)) {
      estado("nenhum Meshtastic por perto");
      espera = 4000;
      continue;
    }
    const int rc = conectar(&alvo);
    if (rc != 0) {
      // A queda da conexao acorda esta tarefa (ver DISCONNECT em ao_gap) - sem
      // escoar esse aviso, a espera acabava na hora e a busca seguinte corria
      // enquanto o no ainda nao voltou a anunciar.
      vTaskDelay(pdMS_TO_TICKS(500));
      ulTaskNotifyTake(pdTRUE, 0);
      espera = (rc > 1) ? portMAX_DELAY : (rc > 0) ? 60000 : 3000;
      continue;
    }

    s_ler = true;   // o que o no ja tinha na queue
    TickType_t ultima = xTaskGetTickCount();
    while (conexao_viva() && !parada_pedida(&s_parada)) {
      ulTaskNotifyTake(pdTRUE, s_ler ? 0 : pdMS_TO_TICKS(1000));
      while (xQueueReceive(s_tx, &s_envio, 0) == pdTRUE) {
        if (escrever(s_h_to, s_envio.d, s_envio.n) != 0) { ESP_LOGW(TAG, "escrita no toRadio falhou"); }
        s_ler = true;   // a resposta vem pelo fromRadio
      }
      if (s_ler || (xTaskGetTickCount() - ultima) > pdMS_TO_TICKS(VOLTA_MS)) {
        s_ler = false;
        ultima = xTaskGetTickCount();
        if (!ler_ate_vazio()) { s_ler = true; }
      }
    }
    s_conectado = false;
    if (!parada_pedida(&s_parada)) { estado("conexão caiu - reconectando"); espera = 2000; }
  }

  if (conexao_viva()) {
    ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    for (int k = 0; k < 20 && conexao_viva(); ++k) { vTaskDelay(pdMS_TO_TICKS(50)); }
  }
  s_conectado = false;
  estado("parado");
  parada_confirmar(&s_parada);
  // Retorna: a vaga do pool devolve o dono e volta a dormir (kernel/task_pool.h).
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
bool mesh_ble_iniciar(void)
{
  if (parada_rodando(&s_parada)) { return true; }
  if (!s_op) {
    s_op = xSemaphoreCreateBinary();
    s_tx = xQueueCreateWithCaps(4, sizeof(envio_t), MALLOC_CAP_SPIRAM);
    s_rx = xMessageBufferCreateWithCaps(8192, MALLOC_CAP_SPIRAM);
    ble_uuid_from_str(&s_u_svc,  TAB5_BLE_UUID_MESHTASTIC);
    ble_uuid_from_str(&s_u_to,   UUID_TORADIO);
    ble_uuid_from_str(&s_u_from, UUID_FROMRADIO);
    ble_uuid_from_str(&s_u_num,  UUID_FROMNUM);
  }
  if (!s_op || !s_tx || !s_rx) { ESP_LOGE(TAG, "sem memoria"); return false; }
  if (s_notif < 0) { s_notif = notif_registrar("Bluetooth"); notif_associar_app(s_notif, "Mesh"); }
  xQueueReset(s_tx);
  xMessageBufferReset(s_rx);
  s_conectado = false; s_conectou_agora = false;
  s_nome[0] = s_end[0] = 0;
  if (!parada_preparar(&s_parada)) { return false; }
  pool_id_t id;
  if (!pool_criar(&id, vTaskMeshBle, nullptr, "mesh_ble", 4096, 3, DONO_SERVICO, "Mesh BLE")) {
    parada_desistir(&s_parada);
    return false;
  }
  s_tarefa = id.handle;
  return true;
}

// A tarefa dorme em esperas PROPRIAS (notificacao, fim de operacao da pilha
// BLE): o aviso e REPETIDO enquanto se espera. Um so podia ser engolido pelo
// xSemaphoreTake(s_op, 0) que antecede cada operacao, e a tarefa ficava presa
// os 12 s do conectar (ou os ~30 s do PIN) - o desligar seguia sem ela e a
// pilha sumia debaixo de um ble_gap_* em curso.
static void cutucar_tarefa(void)
{
  if (s_tarefa) { xTaskNotifyGive(s_tarefa); }
  fim_op(BLE_HS_ENOTCONN);
}

bool mesh_ble_parar(void)
{
  if (!s_tarefa) { return true; }
  if (!parada_pedir(&s_parada, 6000, cutucar_tarefa)) {
    ESP_LOGE(TAG, "a tarefa do enlace nao saiu em 6 s");
    return false;
  }
  s_tarefa = nullptr;
  return true;
}

bool mesh_ble_enviar(const uint8_t* corpo, size_t n)
{
  if (!s_conectado || !s_tx || !corpo || n == 0 || n > CORPO_MAX) { return false; }
  envio_t* e = (envio_t*)heap_caps_malloc(sizeof(envio_t), MALLOC_CAP_SPIRAM);
  if (!e) { return false; }
  e->n = (uint16_t)n;
  memcpy(e->d, corpo, n);
  const bool ok = xQueueSend(s_tx, e, pdMS_TO_TICKS(200)) == pdTRUE;
  heap_caps_free(e);
  if (ok && s_tarefa) { xTaskNotifyGive(s_tarefa); }
  return ok;
}

size_t mesh_ble_receber(uint8_t* buf, size_t max, uint32_t espera_ms)
{
  if (!s_rx) { vTaskDelay(pdMS_TO_TICKS(espera_ms)); return 0; }
  return xMessageBufferReceive(s_rx, buf, max, pdMS_TO_TICKS(espera_ms));
}

bool mesh_ble_conectou_agora(void)
{
  if (!s_conectou_agora) { return false; }
  s_conectou_agora = false;
  return true;
}

bool        mesh_ble_conectado(void) { return s_conectado; }
const char* mesh_ble_descricao(void) { return s_estado; }

bool mesh_ble_rodando(void) { return parada_rodando(&s_parada); }

void mesh_ble_no(char nome[24], char endereco[18])
{
  if (nome)     { snprintf(nome, 24, "%s", s_conectado ? s_nome : ""); }
  if (endereco) { snprintf(endereco, 18, "%s", s_conectado ? s_end : ""); }
}

bool mesh_ble_esperando_pin(void) { return s_pin_esperando; }

bool mesh_ble_digitar_pin(uint32_t pin)
{
  if (!s_pin_esperando) { return false; }
  s_pin_esperando = false;
  if (s_notif >= 0) { notif_limpar(s_notif); }
  struct ble_sm_io io;
  memset(&io, 0, sizeof(io));
  io.action = BLE_SM_IOACT_INPUT;
  io.passkey = pin;
  estado("conferindo o PIN...");
  return ble_sm_inject_io(s_pin_conn, &io) == 0;
}

void mesh_ble_tentar_de_novo(void)
{
  if (parada_rodando(&s_parada)) { xTaskNotifyGive(s_tarefa); }
}
