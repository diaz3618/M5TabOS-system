#include "tab5_ble.hpp"
#include "tab5_listener.hpp"
#include "tab5_signal.hpp"
#include "tab5_wifi.hpp"
#include "tab5_owners.hpp"
#include "task_pool.h"   // turn_on/turn_off use a pool slot instead of a dedicated task

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
#include "tab5_time.hpp"   // tab5_now_ms
#include "tab5_monitor.hpp"   // tab5_mem_marco
#include <cstdio>
#include <cstring>

static const char* TAG = "ble";

extern "C" void ble_store_config_init(void);

// How long to wait for the C6 link to come up (on the first boot, Wi-Fi may be
// disabled and the esp_hosted stack is created here) and for NimBLE to sync
// with its controller.
#define PRAZO_PILHA_MS  20000
#define PRAZO_SYNC_MS    8000


// TURNING ON/OFF WITHOUT A DEDICATED TASK. There used to be a 4 KB internal-RAM
// task that slept forever just to serve these rare requests. Requests now use a
// task-pool slot (kernel/task_pool.h), which exists only while startup runs.
// Ordering is still guaranteed: only one execution runs at a time, and each
// request gets a sequence number so requests arriving during execution are not lost.
static volatile bool     s_quer      = false;   // the last request: on?
static volatile uint32_t s_request    = 0;       // number of last request
static volatile uint32_t s_atendido  = 0;       // number of last served
static volatile bool     s_running   = false;   // there is a vague execution
static bool              s_iniciado  = false;
static SemaphoreHandle_t      s_trava = nullptr;       // the list of seen_devices
static SemaphoreHandle_t      s_sync = nullptr;        // NimBLE synced
// THE STACK IN USE. Scanning/canceling from any task and lowering the BLE task
// go through this mutex. Without it, a screen scan could overlap
// nimble_port_deinit after NimBLE had destroyed its mutex, rebooting the device.
static SemaphoreHandle_t      s_stack = nullptr;
static volatile bool          s_ressincronizar = false; // the stack restarted itself
static volatile tab5_ble_state_t s_estado = TAB5_BLE_OFF;
static volatile bool          s_varrendo = false;
static volatile uint32_t      s_versao = 1;
static bool                   s_stack_nimble = false;  // nimble_port_init done
static ble_uuid_any_t         s_uuid_mesh;

static tab5_ble_seen_t s_vistos[TAB5_BLE_SEEN_MAX];
static int              s_n_vistos = 0;
static int8_t           s_rssi_publicado[TAB5_BLE_SEEN_MAX];   // what the screen has already shown

static void changed(void) { s_versao = s_versao + 1; }

// -------------------------------------------------------------------------------
// STACK
// -------------------------------------------------------------------------------
static void on_synchronize(void)
{
  // Use the public C6 address. Without this step, the first ble_gap_* call
  // returns BLE_HS_ENOADDR.
  ble_hs_util_ensure_addr(0);
  xSemaphoreGive(s_sync);
  // After the stack restarts on its own, no later call will turn the state back
  // to READY; start() finished long ago.
  if (s_ressincronizar && s_estado == TAB5_BLE_STARTING) {
    s_ressincronizar = false;
    s_estado = TAB5_BLE_READY;
    changed();
  }
}

static void on_reiniciar(int reason)
{
  ESP_LOGW(TAG, "the stack restarted (reason %d)", reason);
  s_varrendo = false;
  // While stopping or starting, the execution serving the request owns the state.
  if (s_estado == TAB5_BLE_READY) { s_ressincronizar = true; s_estado = TAB5_BLE_STARTING; }
  changed();
}

static void tarefa_stack(void*)
{
  nimble_port_run();            // so iteration no nimble_port_stop()
  // This task is about to exit. Do not leave its handle registered as an owner:
  // FreeRTOS may reuse it and another task would inherit the "Bluetooth" owner.
  tab5_owner_forget(xTaskGetCurrentTaskHandle());
  nimble_port_freertos_deinit();
}

static bool start(void)
{
  // The C6 must be running. If Wi-Fi is disabled, esp_hosted may not exist yet;
  // the caller already requested tab5_wifi_ensure_stack() from the interface
  // task because it owns I2C. We only wait for it to become ready here.
  for (uint32_t t = 0; !tab5_wifi_stack_ready() && t < PRAZO_PILHA_MS; t += 100) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (!tab5_wifi_stack_ready()) {
    ESP_LOGE(TAG, "the C6 did not start within %d s - Bluetooth unavailable", PRAZO_PILHA_MS / 1000);
    return false;
  }

  if (!s_stack_nimble) {
    esp_hosted_bt_host_stack_cfg_t cfg = ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT();
    esp_err_t err = esp_hosted_bt_host_stack_setup(&cfg);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "the C6 did not provide the BLE controller (%s) - does its firmware lack Bluetooth?",
               esp_err_to_name(err));
      return false;
    }
    tab5_memory_marco("BLE: controlador do C6");
    err = nimble_port_init();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
      esp_hosted_bt_host_stack_teardown();
      return false;
    }

    ble_hs_cfg.sync_cb  = on_synchronize;
    ble_hs_cfg.reset_cb = on_reiniciar;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // SECURITY: Meshtastic requires PIN pairing. The Tab5 is the side that
    // ENTER the PIN (it does not display or have a fixed one): keyboard only.
    // The bond is stored in NVS, so the PIN is requested only the first time.
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();

    s_stack_nimble = true;
    xSemaphoreTake(s_sync, 0);
    nimble_port_freertos_init(tarefa_stack);
    TaskHandle_t h = xTaskGetHandle("nimble_host");
    if (h) { tab5_owner_register(h, OWNER_SERVICE, "Bluetooth"); }
    tab5_memory_marco("NimBLE (stack + tarefa)");
  }

  if (xSemaphoreTake(s_sync, pdMS_TO_TICKS(PRAZO_SYNC_MS)) != pdTRUE && !ble_hs_synced()) {
    ESP_LOGE(TAG, "the stack did not synchronize with the C6 controller");
    return false;
  }
  return true;
}

static bool lower(void)
{
  if (!s_stack_nimble) { return true; }
  xSemaphoreTake(s_stack, portMAX_DELAY);
  if (s_varrendo) { ble_gap_disc_cancel(); s_varrendo = false; }
  bool ok = false;
  // nimble_port_stop() closes open connections and waits for the stack task
  // to leave its loop.
  if (nimble_port_stop() == 0) {
    nimble_port_deinit();
    esp_hosted_bt_host_stack_teardown();
    s_stack_nimble = false;
    ok = true;
  } else {
    ESP_LOGW(TAG, "the stack did not stop - leaving it running");
  }
  xSemaphoreGive(s_stack);
  return ok;
}

// Serves one request; this is the body that used to run in the dedicated task.
static void aplicar(bool turn_on)
{
  if (turn_on) {
    if (s_estado == TAB5_BLE_READY) { return; }
    s_estado = TAB5_BLE_STARTING; changed();
    const bool ok = start();
    s_estado = ok ? TAB5_BLE_READY : TAB5_BLE_FAILED; changed();
    if (ok) { ESP_LOGI(TAG, "Bluetooth ready (NimBLE stack on the P4, C6 radio)"); }
  } else {
    if (s_estado == TAB5_BLE_OFF) { return; }
    if (lower()) {
      s_estado = TAB5_BLE_OFF; changed();
      ESP_LOGI(TAG, "Bluetooth disabled");
    } else {
      s_estado = TAB5_BLE_READY; changed();
    }
  }
}

static void disparar(void);

static void execucao(void*)
{
  uint32_t n;
  do {
    n = s_request;
    aplicar(s_quer);
    s_atendido = n;
  } while (s_atendido != s_request);   // another request arrived: serves it too
  s_running = false;
  // A request that arrived between the loop and clearing s_running may not
  // have started another execution, so check once more.
  if (s_atendido != s_request) { disparar(); }
}

static void disparar(void)
{
  bool free_space = false;
  if (!__atomic_compare_exchange_n(&s_running, &free_space, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    return;   // another execution is already serving this request
  }
  if (!pool_create(nullptr, execucao, nullptr, "ble", 4096, 3, OWNER_SERVICE, "Bluetooth")) {
    s_running = false;
    ESP_LOGE(TAG, "no task-pool slot available to turn Bluetooth on or off");
  }
}

static void request(bool turn_on)
{
  s_quer = turn_on;
  __atomic_add_fetch(&s_request, 1, __ATOMIC_ACQ_REL);
  disparar();
}

// -------------------------------------------------------------------------------
// SCAN
// -------------------------------------------------------------------------------
// TRACKERS AND BEACONS, identified from their public advertising fields.
// Apple (company 0x004C): type 0x12 = Find My network (AirTag and offline
// iPhone/Mac); type 0x02 0x15 = iBeacon, with 1 m RSSI in the last byte.
// Tile: service 0xFEED/0xFEEC.  SmartTag (Samsung): service 0xFD5A.
// Google 0xFEAA: frame 0x40/0x41 = Find My Device network; other frames are
// Eddystone (0x00 and 0x10 carry the 0 m RSSI).
static bool uuid16_anunciado(const struct ble_hs_adv_fields& f, uint16_t u)
{
  for (int k = 0; k < f.num_uuids16; ++k) { if (f.uuids16[k].value == u) { return true; } }
  return f.svc_data_uuid16 && f.svc_data_uuid16_len >= 2
      && (uint16_t)(f.svc_data_uuid16[0] | (f.svc_data_uuid16[1] << 8)) == u;
}

static void classificar(tab5_ble_seen_t* v, const struct ble_hs_adv_fields& f)
{
  const uint8_t* m = f.mfg_data;
  const int mn = f.mfg_data ? f.mfg_data_len : 0;
  auto marcar = [v](uint8_t class_id, const char* brand) {
    v->class_id = class_id;
    snprintf(v->brand, sizeof(v->brand), "%s", brand);
  };
  if (mn >= 3 && m[0] == 0x4C && m[1] == 0x00) {
    if (m[2] == 0x12) { marcar(TAB5_BLE_TRACKER, "Find My"); }
    else if (mn >= 25 && m[2] == 0x02 && m[3] == 0x15) { marcar(TAB5_BLE_BEACON, "iBeacon"); v->p1m = (int8_t)m[24]; }
  }
  if (uuid16_anunciado(f, 0xFEED) || uuid16_anunciado(f, 0xFEEC)) { marcar(TAB5_BLE_TRACKER, "Tile"); }
  if (uuid16_anunciado(f, 0xFD5A)) { marcar(TAB5_BLE_TRACKER, "SmartTag"); }
  if (f.svc_data_uuid16 && f.svc_data_uuid16_len >= 3
      && f.svc_data_uuid16[0] == 0xAA && f.svc_data_uuid16[1] == 0xFE) {
    const uint8_t frame = f.svc_data_uuid16[2];
    if (frame == 0x40 || frame == 0x41) { marcar(TAB5_BLE_TRACKER, "Google"); }
    else {
      marcar(TAB5_BLE_BEACON, "Eddystone");
      if ((frame == 0x00 || frame == 0x10) && f.svc_data_uuid16_len >= 4) { v->p1m = (int8_t)(f.svc_data_uuid16[3] - TAB5_SIGNAL_LOSS_1M); }
    }
  }
  if (v->p1m == 0 && f.tx_pwr_lvl_is_present) { v->p1m = (int8_t)(f.tx_pwr_lvl - TAB5_SIGNAL_LOSS_1M); }
}

static void anotar(const ble_addr_t* a, int8_t rssi, const uint8_t* data, uint8_t n)
{
  struct ble_hs_adv_fields f;
  const bool campos = (ble_hs_adv_parse_fields(&f, data, n) == 0);

  xSemaphoreTake(s_trava, portMAX_DELAY);
  int i = 0;
  for (; i < s_n_vistos; ++i) {
    if (memcmp(s_vistos[i].addr, a->val, 6) == 0) { break; }
  }
  bool novo = false;
  if (i == s_n_vistos) {
    if (s_n_vistos < TAB5_BLE_SEEN_MAX) {
      ++s_n_vistos;
    } else {
      // Full: evict the least recently seen device.
      i = 0;
      for (int k = 1; k < TAB5_BLE_SEEN_MAX; ++k) {
        if (s_vistos[k].seen_ms < s_vistos[i].seen_ms) { i = k; }
      }
    }
    memset(&s_vistos[i], 0, sizeof(s_vistos[i]));
    memcpy(s_vistos[i].addr, a->val, 6);
    novo = true;
  }
  tab5_ble_seen_t* v = &s_vistos[i];
  v->type = a->type;
  v->rssi = novo ? rssi : (int8_t)((v->rssi * 3 + rssi) / 4);
  // The distance shown on screen follows a person walking with the device:
  // a 4 dB change (about 40% in distance) is enough to count as new data.
  if (novo || v->rssi - s_rssi_publicado[i] >= 4 || s_rssi_publicado[i] - v->rssi >= 4) {
    s_rssi_publicado[i] = v->rssi;
    novo = true;
  }
  v->seen_ms = tab5_now_ms();
  if (campos) {
    // The name arrives in the scan response while the service is in the
    // advertisement. Each report contributes part of the record; a report
    // without one field must not erase information already known.
    if (f.name && f.name_len) {
      const size_t k = f.name_len < sizeof(v->name) - 1 ? f.name_len : sizeof(v->name) - 1;
      if (k != strlen(v->name) || memcmp(v->name, f.name, k) != 0) { novo = true; }
      memcpy(v->name, f.name, k);
      v->name[k] = 0;
    }
    // Check the name too: not every firmware puts the service UUID in the
    // advertisement (the 31-byte packet may not have room beside the name),
    // and the default Meshtastic name starts with "Meshtastic".
    if (!v->meshtastic && strncmp(v->name, "Meshtastic", 10) == 0) { v->meshtastic = true; novo = true; }
    for (int k = 0; k < f.num_uuids128; ++k) {
      if (ble_uuid_cmp(&f.uuids128[k].u, &s_uuid_mesh.u) == 0) {
        if (!v->meshtastic) { novo = true; }
        v->meshtastic = true;
      }
    }
    const uint8_t antes = v->class_id;
    classificar(v, f);
    if (v->meshtastic) { v->class_id = TAB5_BLE_MESHTASTIC; }
    if (v->class_id != antes) { novo = true; }
  }
  xSemaphoreGive(s_trava);
  if (novo) { changed(); }
}

static tab5_listener_t<tab5_ble_listener_t> s_ouvinte;   // remove call hold in flight

static int on_evento_gap(struct ble_gap_event* ev, void*)
{
  switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
      anotar(&ev->disc.addr, ev->disc.rssi, ev->disc.data, ev->disc.length_data);
      // Call outside the list mutex. Removing a listener waits for this call
      // to finish (tab5_listener.hpp) before destroying what it uses.
      tab5_listener_call(&s_ouvinte, (const uint8_t*)ev->disc.addr.val, ev->disc.rssi);
      break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
      s_varrendo = false;
      changed();
      break;
    default: break;
  }
  return 0;
}

// -------------------------------------------------------------------------------
// API
// -------------------------------------------------------------------------------
void tab5_ble_init(void)
{
  if (s_iniciado) { return; }
  s_iniciado = true;
  s_trava = xSemaphoreCreateMutex();
  s_sync  = xSemaphoreCreateBinary();
  s_stack = xSemaphoreCreateMutex();
  ble_uuid_from_str(&s_uuid_mesh, TAB5_BLE_UUID_MESHTASTIC);
  // No dedicated task: turn_on/turn_off use the task-pool slot (see trigger).
}

void tab5_ble_turn_on(void)
{
  if (!s_iniciado) { return; }
  tab5_wifi_ensure_stack();   // the radio rail uses I2C, so call from the interface task
  request(true);
}

void tab5_ble_turn_off(void)
{
  if (!s_iniciado) { return; }
  // Immediately mark it stopping, before execution picks up the request, so
  // nobody can see READY and start a new scan or connection.
  if (s_estado != TAB5_BLE_OFF) { s_estado = TAB5_BLE_STOPPING; changed(); }
  request(false);
}

tab5_ble_state_t tab5_ble_state(void) { return s_estado; }
bool tab5_ble_ready(void) { return s_estado == TAB5_BLE_READY; }

const char* tab5_ble_description(void)
{
  switch (s_estado) {
    case TAB5_BLE_OFF: return "disabled";
    case TAB5_BLE_STARTING:   return "starting";
    case TAB5_BLE_READY:    return s_varrendo ? "scanning" : "ready";
    case TAB5_BLE_FAILED:    return "failed (the C6 did not provide Bluetooth)";
    case TAB5_BLE_STOPPING: return "stopping";
  }
  return "?";
}

bool tab5_ble_scan(uint32_t ms)
{
  if (!s_stack || !tab5_ble_ready() || s_varrendo) { return false; }
  // Do not wait: if the stack is stopping, report failure immediately.
  if (xSemaphoreTake(s_stack, 0) != pdTRUE) { return false; }
  if (!tab5_ble_ready() || !s_stack_nimble) { xSemaphoreGive(s_stack); return false; }
  uint8_t proprio;
  if (ble_hs_id_infer_auto(0, &proprio) != 0) { xSemaphoreGive(s_stack); return false; }
  struct ble_gap_disc_params p;
  memset(&p, 0, sizeof(p));
  p.passive = 0;            // active: the name arrives in the scan response
  p.filter_duplicates = 0;  // the name and service arrive in separate reports
  const int rc = ble_gap_disc(proprio, (int32_t)ms, &p, on_evento_gap, nullptr);
  if (rc == 0) { s_varrendo = true; }
  xSemaphoreGive(s_stack);
  if (rc != 0) {
    ESP_LOGW(TAG, "scan rejected (%d)", rc);   // BLE_HS_EBUSY: a connection is active
    return false;
  }
  changed();
  return true;
}

bool tab5_ble_scanning(void) { return s_varrendo; }

void tab5_ble_set_listener(tab5_ble_listener_t f) { tab5_listener_set(&s_ouvinte, f); }

void tab5_ble_stop_scan(void)
{
  if (!s_varrendo || !s_stack) { return; }
  // If the stack is stopping, lower() already cancels the scan; do not wait.
  if (xSemaphoreTake(s_stack, 0) != pdTRUE) { return; }
  if (s_stack_nimble && s_varrendo) { ble_gap_disc_cancel(); }
  s_varrendo = false;
  xSemaphoreGive(s_stack);
  changed();
}

int tab5_ble_seen_count(void) { return s_n_vistos; }

bool tab5_ble_get_seen(int i, tab5_ble_seen_t* out)
{
  if (!s_trava || !out) { return false; }
  xSemaphoreTake(s_trava, portMAX_DELAY);
  const bool ok = (i >= 0 && i < s_n_vistos);
  if (ok) { *out = s_vistos[i]; }
  xSemaphoreGive(s_trava);
  return ok;
}

uint32_t tab5_ble_version(void) { return s_versao; }

void tab5_ble_address_text(const uint8_t a[6], char out[18])
{
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

bool tab5_ble_parse_address(const char* txt, uint8_t a[6])
{
  unsigned b[6];
  if (!txt || sscanf(txt, "%2x:%2x:%2x:%2x:%2x:%2x", &b[5], &b[4], &b[3], &b[2], &b[1], &b[0]) != 6) {
    return false;
  }
  for (int k = 0; k < 6; ++k) { a[k] = (uint8_t)b[k]; }
  return true;
}
