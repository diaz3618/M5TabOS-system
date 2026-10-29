#include "tab5_mesh_ble.hpp"
#include "tab5_time.hpp"   // tab5_now_ms
#include "tab5_ble.hpp"
#include "tab5_owners.hpp"
#include "task_pool.h"
#include "service_stop.hpp"      // cooperative stop of task (kernel)
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

// UUIDs of service of Meshtastic (docs: meshtastic.org, "Bluetooth API").
#define UUID_TORADIO   "f75c76d2-129e-4dad-a1dd-7866124401e7"
#define UUID_FROMRADIO "2c55e69e-4993-11ed-b878-0242ac120002"
#define UUID_FROMNUM   "ed9da18c-a800-4f66-a670-aa7547e34453"

#define CORPO_MAX       512    // MAX_TO_FROM_RADIO_SIZE of firmware
#define PRAZO_OP_MS   10000    // each GATT operation
#define PRAZO_PAR_MS  30000    // pairing (no may be showing PIN)
#define VOLTA_MS       5000    // reading of security, even without warning of fromNum

typedef struct { uint16_t n; uint8_t d[CORPO_MAX]; } send_state_t;

// ---- state ----------------------------------------------------------------
static TaskHandle_t        s_tarefa = nullptr;
static QueueHandle_t       s_tx = nullptr;
static MessageBufferHandle_t s_rx = nullptr;
static SemaphoreHandle_t   s_op = nullptr;        // end of a GATT/GAP operation
static volatile int        s_op_rc = 0;
static stop_t            s_parada = { };      // request for stop / confirmed exit of task
static volatile bool       s_conectado = false, s_conectou_now = false;
static volatile bool       s_read = false;
static volatile uint16_t   s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool       s_pin_request = false;
// PIN ON TIME. Meshtastic has two modes: FIXED PIN (123456 nos
// devices without screen, or whatever has been configured) and RANDOM PIN, which does not show
// in screen EVERY pairing - one saved PIN never gets this one right. Therefore: the
// first attempt uses the PIN of Settings; if the no refuses, the next and
// INTERACTIVE - the request stays waiting (pairing of ~30 s) and Settings >
// Meshtastic opens the keyboard to type what no is showing.
static volatile bool       s_pin_interativo = false;   // next time, ask
static volatile bool       s_pin_esperando = false;    // there is one request waiting for PIN now
static volatile uint16_t   s_pin_conn = 0;
static int                 s_notif = -1;
static char                s_estado[48] = "stopped";
static char                s_name[24] = "", s_end[18] = "";

static ble_uuid_any_t s_u_svc, s_u_to, s_u_from, s_u_num;
static uint16_t s_svc_ini, s_svc_end, s_h_to, s_h_from, s_h_num, s_h_cccd;

// The send_state being written (only the link task uses) - PSRAM, off of stack.
EXT_RAM_BSS_ATTR static send_state_t s_send_state;

// Reading in progress (callback of NimBLE -> task)
static uint8_t  s_lido[CORPO_MAX];
static uint16_t s_lido_n;

// Each change of state also goes to the log: a connection attempt that
// failure leaves the entire trace (searching -> connecting -> pairing -> ...).
static void estado(const char* fmt, const char* arg = nullptr)
{
  char novo[sizeof(s_estado)];
  snprintf(novo, sizeof(novo), fmt, arg ? arg : "");
  if (strcmp(novo, s_estado) != 0) { ESP_LOGI(TAG, "%s", novo); }
  snprintf(s_estado, sizeof(s_estado), "%s", novo);
}

static int wait_op(uint32_t ms)
{
  if (stop_requested(&s_parada)) { return BLE_HS_ENOTCONN; }   // asked to stop: no new operation
  if (xSemaphoreTake(s_op, pdMS_TO_TICKS(ms)) != pdTRUE) { return BLE_HS_ETIMEOUT; }
  return s_op_rc;
}
static void end_op(int rc) { s_op_rc = rc; xSemaphoreGive(s_op); }

// -------------------------------------------------------------------------------
// CONNECTION EVENTS (task of NimBLE - short, just write it down and wake up)
// -------------------------------------------------------------------------------
static int on_gap(struct ble_gap_event* ev, void*)
{
  switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (ev->connect.status == 0) { s_conn = ev->connect.conn_handle; }
      end_op(ev->connect.status);
      break;

    case BLE_GAP_EVENT_DISCONNECT:
      ESP_LOGI(TAG, "desconectado (motivo 0x%x)", ev->disconnect.reason);
      s_conn = BLE_HS_CONN_HANDLE_NONE;
      s_conectado = false;
      end_op(BLE_HS_ENOTCONN);   // unlocks those waiting for an operation
      if (s_tarefa) { xTaskNotifyGive(s_tarefa); }
      break;

    case BLE_GAP_EVENT_ENC_CHANGE:
      end_op(ev->enc_change.status);
      break;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
      // The PIN. The no says what it wants: TYPE (the common one - it shows or has a
      // fixed, 123456 without screen) or CHECK one number (we accept).
      struct ble_sm_io io;
      memset(&io, 0, sizeof(io));
      io.action = ev->passkey.params.action;
      if (io.action == BLE_SM_IOACT_INPUT) {
        s_pin_request = true;
        if (s_pin_interativo || settings_mesh_pin_on_screen()) {
          s_pin_interativo = true;
          // Wait for whoever types (mesh_ble_digitar_pin): nothing is injected here.
          s_pin_conn = ev->passkey.conn_handle;
          s_pin_esperando = true;
          estado("enter the PIN shown by the node");
          if (s_notif >= 0) { notification_publicar(s_notif, 1, NOTIF_ATENCAO, "PIN do Meshtastic"); }
        ESP_LOGI(TAG, "the node requested the PIN - waiting for the user (Settings > Meshtastic)");
          break;
        }
        io.passkey = settings_mesh_pin();
        ESP_LOGI(TAG, "the node requested the PIN - using the one from Settings");
      } else if (io.action == BLE_SM_IOACT_NUMCMP) {
        io.numcmp_accept = 1;
      }
      ble_sm_inject_io(ev->passkey.conn_handle, &io);
      break;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      // If you haven't forgotten the link (reset) and want to pair again: we erase the
      // ours and let us repeat it - otherwise we would be refusing forever.
      struct ble_gap_conn_desc d;
      if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) {
        ble_store_util_delete_peer(&d.peer_id_addr);
      }
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_NOTIFY_RX:
      if (ev->notify_rx.attr_handle == s_h_num) {
        s_read = true;
        if (s_tarefa) { xTaskNotifyGive(s_tarefa); }
      }
      break;

    default: break;
  }
  return 0;
}

// -------------------------------------------------------------------------------
// SYNCHRONOUS GATT - one operation at a time, always of task of link
// -------------------------------------------------------------------------------
static int cb_mtu(uint16_t, const struct ble_gatt_error* e, uint16_t mtu, void*)
{
  if (e->status == 0) { ESP_LOGI(TAG, "MTU %u", (unsigned)mtu); }
  end_op(e->status);
  return 0;
}

static int cb_svc(uint16_t, const struct ble_gatt_error* e, const struct ble_gatt_svc* s, void*)
{
  if (e->status == 0) { s_svc_ini = s->start_handle; s_svc_end = s->end_handle; return 0; }
  end_op(e->status == BLE_HS_EDONE ? 0 : e->status);
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
  end_op(e->status == BLE_HS_EDONE ? 0 : e->status);
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
  end_op(e->status == BLE_HS_EDONE ? 0 : e->status);
  return 0;
}

static int cb_read(uint16_t, const struct ble_gatt_error* e, struct ble_gatt_attr* a, void*)
{
  if (e->status == 0 && a) {
    const uint16_t n = OS_MBUF_PKTLEN(a->om);
    if (a->offset + n <= CORPO_MAX) {
      os_mbuf_copydata(a->om, 0, n, s_lido + a->offset);
      s_lido_n = a->offset + n;
    }
    return 0;
  }
  end_op(e->status == BLE_HS_EDONE ? 0 : e->status);
  return 0;
}

static int cb_write(uint16_t, const struct ble_gatt_error* e, struct ble_gatt_attr*, void*)
{
  end_op(e->status);
  return 0;
}

static bool connection_alive(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }

static int write(uint16_t h, const uint8_t* d, size_t n)
{
  if (!connection_alive()) { return BLE_HS_ENOTCONN; }
  xSemaphoreTake(s_op, 0);
  const uint16_t mtu = ble_att_mtu(s_conn);
  int rc;
  if (n + 3 <= mtu) {
    rc = ble_gattc_write_flat(s_conn, h, d, (uint16_t)n, cb_write, nullptr);
  } else {
    struct os_mbuf* om = ble_hs_mbuf_from_flat(d, (uint16_t)n);
    rc = om ? ble_gattc_write_long(s_conn, h, 0, om, cb_write, nullptr) : BLE_HS_ENOMEM;
  }
  return rc ? rc : wait_op(PRAZO_OP_MS);
}

// FromRadio: one message per reading; empty reading = there is nothing left.
// Up to 64 at a time (the of send_state queue cannot wait for the entire dump). Return
// false when stopped at the ceiling WITHOUT emptying: the rest comes in next iteration, in
// time - before I expected the next warning of fromNum (which only comes with the package
// NEW) or the 5 s of reading of security, and one dump of many was at our disposal
// hiccups.
static bool read_ate_vazio(void)
{
  for (int k = 0; k < 64 && connection_alive() && !stop_requested(&s_parada); ++k) {
    xSemaphoreTake(s_op, 0);
    s_lido_n = 0;
    if (ble_gattc_read_long(s_conn, s_h_from, 0, cb_read, nullptr) != 0) { return true; }
    if (wait_op(PRAZO_OP_MS) != 0) { return true; }
    if (s_lido_n == 0) { return true; }
    if (xMessageBufferSend(s_rx, s_lido, s_lido_n, pdMS_TO_TICKS(500)) == 0) {
      ESP_LOGW(TAG, "the Mesh service is not consuming data - %u-byte message lost",
               (unsigned)s_lido_n);
    }
  }
  return !connection_alive() || stop_requested(&s_parada);
}

// -------------------------------------------------------------------------------
// FIND, CONNECT, PAIR, DISCOVER
// -------------------------------------------------------------------------------
static bool achar_no(ble_addr_t* alvo)
{
  char salvo[18];
  settings_mesh_ble_address(salvo);
  uint8_t quer[6];
  const bool tem_salvo = tab5_ble_parse_address(salvo, quer);

  estado(tem_salvo ? "procurando %s" : "procurando um no Meshtastic", salvo);
  // If the screen is already scanning, we take advantage of its scanning.
  // 8 s: with Wi-Fi connected, the C6 divides the radio, and the search hears less.
  if (!tab5_ble_scanning() && !tab5_ble_scan(8000)) { return false; }
  while (tab5_ble_scanning() && !stop_requested(&s_parada)) { vTaskDelay(pdMS_TO_TICKS(200)); }

  int melhor = -1;
  int8_t rssi = -128;
  tab5_ble_seen_t v;
  const uint32_t now = tab5_now_ms();
  for (int i = 0; i < tab5_ble_seen_count(); ++i) {
    if (!tab5_ble_get_seen(i, &v) || now - v.seen_ms > 15000) { continue; }
    if (tem_salvo ? (memcmp(v.addr, quer, 6) == 0) : (v.meshtastic && v.rssi > rssi)) {
      melhor = i; rssi = v.rssi;
      if (tem_salvo) { break; }
    }
  }
  if (melhor < 0) { return false; }
  tab5_ble_get_seen(melhor, &v);
  alvo->type = v.type;
  memcpy(alvo->val, v.addr, 6);
  snprintf(s_name, sizeof(s_name), "%s", v.name[0] ? v.name : "Meshtastic");
  tab5_ble_address_text(v.addr, s_end);
  return true;
}

// 0 = connected and ready; <0 = try again soon; >0 = wait longer (PIN)
static int conectar(const ble_addr_t* alvo)
{
  uint8_t proprio;
  if (ble_hs_id_infer_auto(0, &proprio) != 0) { return -1; }

  estado("conectando a %s", s_name);
  xSemaphoreTake(s_op, 0);
  int rc = ble_gap_connect(proprio, alvo, 10000, nullptr, on_gap, nullptr);
  if (rc != 0) { ESP_LOGW(TAG, "ble_gap_connect: %d", rc); return -1; }
  rc = wait_op(12000);
  if (rc != 0 || !connection_alive()) {
      ESP_LOGW(TAG, "connection failed (%d)", rc);
    ble_gap_conn_cancel();
    return -1;
  }

  // The addresses of this connection: what the stack thinks is ours (id), what
  // was of fact on ar (ota) and the of no. The final check of pairing
  // safe includes addresses - if id and ota diverge, that's where it breaks.
  {
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(s_conn, &d) == 0) {
      char a1[18], a2[18], a3[18];
      tab5_ble_address_text(d.our_id_addr.val, a1);
      tab5_ble_address_text(d.our_ota_addr.val, a2);
      tab5_ble_address_text(d.peer_ota_addr.val, a3);
      ESP_LOGI(TAG, "enderecos: nosso id %s (tipo %u), no ar %s (tipo %u), no %s (tipo %u)",
               a1, d.our_id_addr.type, a2, d.our_ota_addr.type, a3, d.peer_ota_addr.type);
    }
  }

  xSemaphoreTake(s_op, 0);
  if (ble_gattc_exchange_mtu(s_conn, cb_mtu, nullptr) == 0) { wait_op(PRAZO_OP_MS); }

  // Pairing. Already linked: just resume the cipher, without PIN. No link: the no
  // asks for the PIN (see PASSKEY_ACTION).
  estado("pareando com %s", s_name);
  s_pin_request = false;
  xSemaphoreTake(s_op, 0);
  rc = ble_gap_security_initiate(s_conn);
  if (rc == 0) { rc = wait_op(PRAZO_PAR_MS); }
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    struct ble_gap_conn_desc d;
    const bool achou = connection_alive() && ble_gap_conn_find(s_conn, &d) == 0;
    if (connection_alive()) { ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM); }
    if (s_pin_esperando) {
      s_pin_esperando = false;
      if (s_notif >= 0) { notification_limpar(s_notif); }
    }
    // DHKEY CHECK FAILED: 20 rounds of PIN passed (PIN was correct),
    // but the final check of secure pairing (LE Secure Connections)
    // didn't hit. LEGACY PIN mode does not have this step, and Meshtastic does
    // accepts: switch to it and ask for the new PIN now (it doesn't show another one).
    if ((rc == BLE_HS_SM_PEER_ERR(BLE_SM_ERR_DHKEY) || rc == BLE_HS_SM_US_ERR(BLE_SM_ERR_DHKEY))
     && ble_hs_cfg.sm_sc) {
      ESP_LOGW(TAG, "verificacao final do pareamento seguro falhou (%d) - tentando o pareamento legado", rc);
      ble_hs_cfg.sm_sc = 0;
      s_pin_interativo = true;
      estado("trying another pairing mode - the node will show another PIN");
      return -1;
    }
    // THE SAVED PIN WAS REFUSED: no shows a new PIN with each pairing.
    // Remember this (NVS) and try again now asking for the PIN.
    if (s_pin_request && !s_pin_interativo) {
      ESP_LOGW(TAG, "saved PIN rejected (%d) - the node uses the on-screen PIN; asking from now on", rc);
      settings_set_mesh_pin_on_screen(true);
      settings_set_mesh_pin(123456);   // the save was wrong: iteration on default
      s_pin_interativo = true;
      estado("the node shows the PIN on screen - trying again");
      return -1;
    }
    if (s_pin_request) {
      // Wrong PIN (or the deadline has passed). Do not repeat alone: each attempt
      // bothers the node and generates a new PIN on it. The next one asks for the PIN in
      // time, and only starts when the user request (mesh_ble_try_de_novo, which
      // the SEND of PIN in Settings > Meshtastic calls).
      ESP_LOGW(TAG, "pareamento recusado (%d) - PIN errado ou prazo vencido", rc);
      estado("PIN rejected - tap the node to try again");
      s_pin_interativo = true;
      return 2;
    }
    // No PIN request and it still failed: the saved link is no longer valid
    // (no has been reset). Delete and try of zero in next iteration.
    if (achou) { ble_store_util_delete_peer(&d.peer_id_addr); }
    ESP_LOGW(TAG, "cifra recusada (%d) - vinculo antigo apagado", rc);
    return -1;
  }

  // Discovery of the service and the three characteristics.
  s_svc_ini = s_svc_end = s_h_to = s_h_from = s_h_num = s_h_cccd = 0;
  xSemaphoreTake(s_op, 0);
  rc = ble_gattc_disc_svc_by_uuid(s_conn, &s_u_svc.u, cb_svc, nullptr);
  if (rc == 0) { rc = wait_op(PRAZO_OP_MS); }
  if (rc == 0 && s_svc_ini) {
    xSemaphoreTake(s_op, 0);
    rc = ble_gattc_disc_all_chrs(s_conn, s_svc_ini, s_svc_end, cb_chr, nullptr);
    if (rc == 0) { rc = wait_op(PRAZO_OP_MS); }
  }
  if (rc == 0 && s_h_num) {
    xSemaphoreTake(s_op, 0);
    rc = ble_gattc_disc_all_dscs(s_conn, s_h_num, s_svc_end, cb_dsc, nullptr);
    if (rc == 0) { rc = wait_op(PRAZO_OP_MS); }
  }
  if (rc != 0 || !s_h_to || !s_h_from) {
    ESP_LOGW(TAG, "the device does not provide the complete Meshtastic service (%d)", rc);
    if (connection_alive()) { ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM); }
    return -1;
  }

  // Signs fromNum. Without it it still works (the iteration of VOLTA_MS reads like this
  // same), they are slower.
  if (s_h_cccd) {
    const uint8_t liga[2] = { 0x01, 0x00 };
    if (write(s_h_cccd, liga, 2) != 0) { ESP_LOGW(TAG, "could not subscribe to fromNum"); }
  }

  s_pin_interativo = false;
  if (s_notif >= 0) { notification_limpar(s_notif); }

  // From this point forward and this year: guard to reconnect directly.
  settings_set_mesh_ble_address(s_end);
  ESP_LOGI(TAG, "connected to %s (%s)", s_name, s_end);
  estado("connected to %s", s_name);
  s_conectado = true;
  s_conectou_now = true;
  return 0;
}

// -------------------------------------------------------------------------------
// THE LINK TASK
// -------------------------------------------------------------------------------
static void vTaskMeshBle(void*)
{
  uint32_t espera = 0;
  for (;;) {
    if (stop_requested(&s_parada)) { break; }
    if (espera) {
      ulTaskNotifyTake(pdTRUE, espera == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(espera));
      espera = 0;
      continue;
    }

    if (!tab5_ble_ready()) {
      estado(tab5_ble_state() == TAB5_BLE_FAILED ? "Bluetooth failed" : "waiting for Bluetooth");
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
      // Drop of connection wakes up this task (see DISCONNECT in ao_gap) - without
      // this warning was passed, the wait ended instantly and the next search took place
      // while no has yet to announce again.
      vTaskDelay(pdMS_TO_TICKS(500));
      ulTaskNotifyTake(pdTRUE, 0);
      espera = (rc > 1) ? portMAX_DELAY : (rc > 0) ? 60000 : 3000;
      continue;
    }

    s_read = true;   // what the no already had in queue
    TickType_t ultima = xTaskGetTickCount();
    while (connection_alive() && !stop_requested(&s_parada)) {
      ulTaskNotifyTake(pdTRUE, s_read ? 0 : pdMS_TO_TICKS(1000));
      while (xQueueReceive(s_tx, &s_send_state, 0) == pdTRUE) {
        if (write(s_h_to, s_send_state.d, s_send_state.n) != 0) { ESP_LOGW(TAG, "writing to toRadio failed"); }
        s_read = true;   // the answer comes from fromRadio
      }
      if (s_read || (xTaskGetTickCount() - ultima) > pdMS_TO_TICKS(VOLTA_MS)) {
        s_read = false;
        ultima = xTaskGetTickCount();
        if (!read_ate_vazio()) { s_read = true; }
      }
    }
    s_conectado = false;
    if (!stop_requested(&s_parada)) { estado("connection dropped - reconnecting"); espera = 2000; }
  }

  if (connection_alive()) {
    ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    for (int k = 0; k < 20 && connection_alive(); ++k) { vTaskDelay(pdMS_TO_TICKS(50)); }
  }
  s_conectado = false;
  estado("stopped");
  stop_confirm(&s_parada);
  // Returns: the vacancy of pool returns the owner and iteration to sleep (kernel/task_pool.h).
}

// -------------------------------------------------------------------------------
// API
// -------------------------------------------------------------------------------
bool mesh_ble_start(void)
{
  if (stop_running(&s_parada)) { return true; }
  if (!s_op) {
    s_op = xSemaphoreCreateBinary();
    s_tx = xQueueCreateWithCaps(4, sizeof(send_state_t), MALLOC_CAP_SPIRAM);
    s_rx = xMessageBufferCreateWithCaps(8192, MALLOC_CAP_SPIRAM);
    ble_uuid_from_str(&s_u_svc,  TAB5_BLE_UUID_MESHTASTIC);
    ble_uuid_from_str(&s_u_to,   UUID_TORADIO);
    ble_uuid_from_str(&s_u_from, UUID_FROMRADIO);
    ble_uuid_from_str(&s_u_num,  UUID_FROMNUM);
  }
  if (!s_op || !s_tx || !s_rx) { ESP_LOGE(TAG, "out of memory"); return false; }
  if (s_notif < 0) { s_notif = notification_registrar("Bluetooth"); notification_associar_app(s_notif, "Mesh"); }
  xQueueReset(s_tx);
  xMessageBufferReset(s_rx);
  s_conectado = false; s_conectou_now = false;
  s_name[0] = s_end[0] = 0;
  if (!stop_prepare(&s_parada)) { return false; }
  pool_id_t id;
  if (!pool_create(&id, vTaskMeshBle, nullptr, "mesh_ble", 4096, 3, OWNER_SERVICE, "Mesh BLE")) {
    stop_abandon(&s_parada);
    return false;
  }
  s_tarefa = id.handle;
  return true;
}

// The task sleeps on OWN waits (notification, end of operation of stack
// BLE): the warning is REPEATED while waiting. One could be swallowed by the
// xSemaphoreTake(s_op, 0) that precedes each operation, and the task was stuck
// the 12 s of connecting (or the ~30 s of PIN) - the disable continued without it and the
// stack disappeared under one ble_gap_* in progress.
static void cutucar_tarefa(void)
{
  if (s_tarefa) { xTaskNotifyGive(s_tarefa); }
  end_op(BLE_HS_ENOTCONN);
}

bool mesh_ble_stop(void)
{
  if (!s_tarefa) { return true; }
  if (!stop_request(&s_parada, 6000, cutucar_tarefa)) {
    ESP_LOGE(TAG, "the link task did not exit within 6 s");
    return false;
  }
  s_tarefa = nullptr;
  return true;
}

bool mesh_ble_send(const uint8_t* body, size_t n)
{
  if (!s_conectado || !s_tx || !body || n == 0 || n > CORPO_MAX) { return false; }
  send_state_t* e = (send_state_t*)heap_caps_malloc(sizeof(send_state_t), MALLOC_CAP_SPIRAM);
  if (!e) { return false; }
  e->n = (uint16_t)n;
  memcpy(e->d, body, n);
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

bool mesh_ble_conectou_now(void)
{
  if (!s_conectou_now) { return false; }
  s_conectou_now = false;
  return true;
}

bool        mesh_ble_conectado(void) { return s_conectado; }
const char* mesh_ble_descricao(void) { return s_estado; }

bool mesh_ble_running(void) { return stop_running(&s_parada); }

void mesh_ble_no(char name[24], char endereco[18])
{
  if (name)     { snprintf(name, 24, "%s", s_conectado ? s_name : ""); }
  if (endereco) { snprintf(endereco, 18, "%s", s_conectado ? s_end : ""); }
}

bool mesh_ble_esperando_pin(void) { return s_pin_esperando; }

bool mesh_ble_digitar_pin(uint32_t pin)
{
  if (!s_pin_esperando) { return false; }
  s_pin_esperando = false;
  if (s_notif >= 0) { notification_limpar(s_notif); }
  struct ble_sm_io io;
  memset(&io, 0, sizeof(io));
  io.action = BLE_SM_IOACT_INPUT;
  io.passkey = pin;
  estado("conferindo o PIN...");
  return ble_sm_inject_io(s_pin_conn, &io) == 0;
}

void mesh_ble_tentar_of_novo(void)
{
  if (stop_running(&s_parada)) { xTaskNotifyGive(s_tarefa); }
}
