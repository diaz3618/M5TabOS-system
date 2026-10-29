// Meshtastic configuration - see the big warning in tab5_mesh_admin.hpp.
#include "tab5_mesh_admin.hpp"
#include "mesh_pb.hpp"   // the codec of service, one only for reading and writing

#include <string.h>
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "app.h"          // system_warn
#include "tab5_gps.hpp"
#include "tab5_time.hpp"   // tab5_now_ms
#include "settings.h"       // SETTINGS_MESH_TRANSPORT_BLUETOOTH

static const char* TAG = "mesh_admin";

// -------------------------------------------------------------------------------
// Provided by the service (tab5_mesh.cpp) - not part of the public API.
// -------------------------------------------------------------------------------
bool tab5_mesh_interno_admin(const uint8_t* adm, size_t n);   // AdminMessage to our no
bool tab5_mesh_interno_pode_admin(void);                      // on the other hand, dump complete
void tab5_mesh_interno_reapresentar_em(uint32_t ms, bool reiniciou);   // request the configuration in a moment

// AdminMessage - field numbers checked in admin.pb.h (2.5 to current).
#define ADM_SET_OWNER          32
#define ADM_SET_CHANNEL        33
#define ADM_SET_CONFIG         34
#define ADM_SET_MODULE_CONFIG  35
#define ADM_SET_FIXED_POSITION 41
#define ADM_REMOVE_FIXED_POS   42
#define ADM_SET_TIME_ONLY      43
#define ADM_BEGIN_EDIT         64
#define ADM_COMMIT_EDIT        65
#define ADM_REBOOT_SECONDS     97

// Data.payload max_size (mesh.pb.h): the entire AdminMessage has to fit.
#define ADM_MAX 233

// Sections: Config 1..10 (device, position, power, network, display, lora,
// bluetooth, security, sessionkey, device_ui) and ModuleConfig 1..17.
#define CFG_SECOES 11
#define MOD_SECOES 18
#define BRUTO_MAX  256

typedef struct { bool tem; uint16_t n; uint8_t b[BRUTO_MAX]; } bruto_t;

// What each editable field is in protobuf. All varint.
typedef struct { bool modulo; uint8_t secao; uint8_t num; } field_def_t;
static const field_def_t CAMPOS[MESH_CFG_N] = {
  /* PAPER       */ { false, 1,  1 },   // DeviceConfig.role
  /* NODEINFO_S  */ { false, 1,  7 },   // DeviceConfig.node_info_broadcast_secs
  /* POS_S       */ { false, 2,  1 },   // PositionConfig.position_broadcast_secs
  /* HEELS      */ { false, 6,  8 },   // LoRaConfig.hop_limit
  /* TX_DBM      */ { false, 6, 10 },   // LoRaConfig.tx_power (int32)
  /* BT          */ { false, 7,  1 },   // BluetoothConfig.enabled
  /* MQTT        */ { true,  1,  1 },   // ModuleConfig.MQTTConfig.enabled
  /* TELE_S      */ { true,  6,  1 },   // ModuleConfig.TelemetryConfig.device_update_interval
  /* MQTT_PROXY  */ { true,  1,  9 },   // ModuleConfig.MQTTConfig.proxy_to_client_enabled
  /* POS_INTELLIG */ { false, 2,  2 },   // PositionConfig.position_broadcast_smart_enabled
  /* GPS_MODE    */ { false, 2, 13 },   // PositionConfig.gps_mode
};

// Sections that are NEVER resent (see the .hpp): power 3, network 4,
// security 8, sessionkey 9; serial module 2.
static bool secao_proibida(bool modulo, int secao)
{
  return modulo ? (secao == 2) : (secao == 3 || secao == 4 || secao == 8 || secao == 9);
}

// Written by the service task, read by the interface: spinlock, because the
// critical sections are copies and scans of a few hundred bytes.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static bruto_t s_cfg[CFG_SECOES];
EXT_RAM_BSS_ATTR static bruto_t s_mod[MOD_SECOES];
EXT_RAM_BSS_ATTR static bruto_t s_user, s_meta, s_myinfo;
EXT_RAM_BSS_ATTR static bruto_t s_canal0;   // the entire PRIMARY Channel (index 0)
static char     s_aviso[96];
static uint32_t s_aviso_ms = 0;
static bool     s_dump_ok = false;
static bool     s_gravando = false;
static uint32_t s_gravando_ms = 0;
static uint32_t s_versao = 0;   // read without lock: only "changed?", like other versions of service
static char     s_reason[72];   // why the last mesh_cfg_gravar didn't send anything (to the screen)

#define GRAVANDO_PRAZO_MS 90000   // no restarts in ~5 s; 90s covers one slow boot


// -------------------------------------------------------------------------------
// Service hooks
// -------------------------------------------------------------------------------
static void guardar(bruto_t* d, const uint8_t* b, size_t n)
{
  if (n > BRUTO_MAX) {
    // Larger than buffer: saving truncated and resending later would lose fields
    // at no. Mark as unknown - her edit remains blocked.
    ESP_LOGD(TAG, "secao de %u bytes nao cabe - edicao dela desligada", (unsigned)n);
    taskENTER_CRITICAL(&s_mux); d->tem = false; d->n = 0; taskEXIT_CRITICAL(&s_mux);
    return;
  }
  taskENTER_CRITICAL(&s_mux);
  memcpy(d->b, b, n); d->n = (uint16_t)n; d->tem = true;
  taskEXIT_CRITICAL(&s_mux);
  ++s_versao;
}

void mesh_admin_guardar_secao(bool modulo, int secao, const uint8_t* b, size_t n)
{
  if (modulo) { if (secao >= 1 && secao < MOD_SECOES) { guardar(&s_mod[secao], b, n); } }
  else        { if (secao >= 1 && secao < CFG_SECOES) { guardar(&s_cfg[secao], b, n); } }
}
void mesh_admin_guardar_user(const uint8_t* b, size_t n)     { guardar(&s_user, b, n); }
void mesh_admin_guardar_metadata(const uint8_t* b, size_t n) { guardar(&s_meta, b, n); }
void mesh_admin_guardar_my_info(const uint8_t* b, size_t n)  { guardar(&s_myinfo, b, n); }
void mesh_admin_guardar_canal(int idx, const uint8_t* b, size_t n) { if (idx == 0) { guardar(&s_canal0, b, n); } }

void mesh_admin_guardar_aviso(const uint8_t* b, size_t n)
{
  char msg[sizeof(s_aviso)] = "";
  if (!pb_achar_text(b, n, 4, msg, sizeof(msg)) || !msg[0]) { return; }   // ClientNotification.message
  ESP_LOGW(TAG, "aviso do no: %s", msg);
  taskENTER_CRITICAL(&s_mux);
  memcpy(s_aviso, msg, sizeof(s_aviso));
  s_aviso_ms = tab5_now_ms() | 1u;
  taskEXIT_CRITICAL(&s_mux);
  ++s_versao;
  system_notify(AVISO_MESH_NOS);
}

void mesh_admin_start_dump(void) { s_dump_ok = false; ++s_versao; }

void mesh_admin_end_dump(void)
{
  s_dump_ok = true;
  if (s_gravando) { ESP_LOGI(TAG, "node checked in again - new values confirmed"); }
  s_gravando = false;
  ++s_versao;
  system_notify(AVISO_MESH_NOS);
}

// INSISTS while no iteration. The first request of configuration exit 15 s
// after of record (tab5_mesh_interno_represent_em); one boot slower
// loses this request, and without this the next would only come out at the rate of silence of
// service (minutes). Every 20 seconds until the dump arrives or the deadline ends.
void mesh_admin_tique(uint32_t t)
{
  static uint32_t ultimo = 0;
  if (!s_gravando) { ultimo = 0; return; }
  // THE DEADLINE APPLIES HERE TOO, and not just when the screen asks (state()):
  // with the page closed, one in which never iteration would be request of 20 in 20 s
  // forever.
  if (t - s_gravando_ms > GRAVANDO_PRAZO_MS) {
    ESP_LOGW(TAG, "the node did not check in again within %d s after saving", GRAVANDO_PRAZO_MS / 1000);
    s_gravando = false;
    ++s_versao;
    return;
  }
  if (t - s_gravando_ms < 35000) { return; }
  if (ultimo == 0 || t - ultimo >= 20000) {
    ultimo = t;
    ESP_LOGI(TAG, "the node has not returned yet - requesting configuration again");
    tab5_mesh_interno_reapresentar_em(0, true);
  }
}

// -------------------------------------------------------------------------------
// Reading
// -------------------------------------------------------------------------------
// CHANNEL POSITION ACCURACY: Channel{ 2: settings{ 7: module_settings{
// 1: position_precision } } }. Without module_settings the firmware handles the
// primary as the channels of before of field exist: 13 bits (~3 km) - see
// PositionModule::allocPositionPacket. With lock socket.
static int32_t precisao_of(const bruto_t& c)
{
  if (!c.tem) { return -1; }
  pb_field_t st, mod;
  if (!pb_achar(c.b, c.n, 2, PB_LEN, &st)) { return 13; }
  if (!pb_achar(st.val, st.n_val, 7, PB_LEN, &mod)) { return 13; }
  uint64_t v;
  return pb_achar_varint(mod.val, mod.n_val, 1, &v) ? (int32_t)v : 0;
}

static mesh_cfg_estado_t estado(void)
{
  if (s_gravando && (tab5_now_ms() - s_gravando_ms) > GRAVANDO_PRAZO_MS) {
    ESP_LOGW(TAG, "the node did not check in again within %d s after saving", GRAVANDO_PRAZO_MS / 1000);
    s_gravando = false;
  }
  if (s_gravando) { return MESH_CFG_GRAVANDO; }
  return (s_dump_ok && tab5_mesh_interno_pode_admin()) ? MESH_CFG_PRONTO : MESH_CFG_SEM_NO;
}

uint32_t mesh_cfg_versao(void) { return s_versao; }

void mesh_cfg_read(mesh_cfg_t* o)
{
  memset(o, 0, sizeof(*o));
  o->state = (uint8_t)estado();
  o->regiao = -1;
  o->nos_no_radio = -1;
  uint64_t v;
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < MESH_CFG_N; ++i) {
    const field_def_t& c = CAMPOS[i];
    const bruto_t& b = c.modulo ? s_mod[c.secao] : s_cfg[c.secao];
    o->conhecido[i] = b.tem;
    o->v[i] = (b.tem && pb_achar_varint(b.b, b.n, c.num, &v)) ? (int32_t)v : 0;
  }
  if (s_cfg[2].tem && pb_achar_varint(s_cfg[2].b, s_cfg[2].n, 3, &v)) { o->pos_fixa = (v != 0); }
  o->precisao = precisao_of(s_canal0);
  if (s_cfg[6].tem) {
    o->regiao     = pb_achar_varint(s_cfg[6].b, s_cfg[6].n, 7, &v) ? (int32_t)v : 0;
    o->preset     = pb_achar_varint(s_cfg[6].b, s_cfg[6].n, 2, &v) ? (int32_t)v : 0;
    o->usa_preset = pb_achar_varint(s_cfg[6].b, s_cfg[6].n, 1, &v) && v != 0;
  }
  if (s_cfg[4].tem) {
    o->wifi_conhecido = true;
    o->wifi_enabled = pb_achar_varint(s_cfg[4].b, s_cfg[4].n, 1, &v) && v != 0;
    pb_achar_text(s_cfg[4].b, s_cfg[4].n, 3, o->wifi_ssid, sizeof(o->wifi_ssid));
  }
  if (s_meta.tem) {
    pb_achar_text(s_meta.b, s_meta.n, 1, o->firmware, sizeof(o->firmware));
    if (pb_achar_varint(s_meta.b, s_meta.n, 9, &v)) { o->hw = (uint8_t)v; }
    o->tem_wifi = pb_achar_varint(s_meta.b, s_meta.n, 4, &v) && v;
    o->tem_bt   = pb_achar_varint(s_meta.b, s_meta.n, 5, &v) && v;
  }
  if (s_myinfo.tem) {
    if (pb_achar_varint(s_myinfo.b, s_myinfo.n, 8, &v))  { o->reinicios = (uint32_t)v; }
    // nodedb_count (15) only exists in the new firmware: absent stays -1, and the screen
    // doesn't invent a number.
    if (pb_achar_varint(s_myinfo.b, s_myinfo.n, 15, &v)) { o->nos_no_radio = (int32_t)v; }
  }
  memcpy(o->aviso, s_aviso, sizeof(o->aviso));
  o->notice_ms = s_aviso_ms;
  taskEXIT_CRITICAL(&s_mux);
}

bool mesh_admin_no_tem_position_propria(void)
{
  uint64_t fixa = 0, gps = 0;
  taskENTER_CRITICAL(&s_mux);
  if (s_cfg[2].tem) {
    pb_achar_varint(s_cfg[2].b, s_cfg[2].n, 3, &fixa);    // fixed_position
    pb_achar_varint(s_cfg[2].b, s_cfg[2].n, 13, &gps);    // gps_mode (1 = ENABLED)
  }
  taskEXIT_CRITICAL(&s_mux);
  return fixa != 0 || gps == 1;
}

int32_t mesh_admin_precisao_primario(void)
{
  taskENTER_CRITICAL(&s_mux);
  const int32_t p = precisao_of(s_canal0);
  taskEXIT_CRITICAL(&s_mux);
  return p;
}

// -------------------------------------------------------------------------------
// Writing
// -------------------------------------------------------------------------------
// AdminMessage{ field: varint value }
static bool admin_varint(int field, uint64_t valor)
{
  uint8_t a[16];
  pb_escritor_t w = pb_escritor(a, sizeof(a));
  pb_w_num(&w, field, valor);
  return tab5_mesh_interno_admin(a, pb_tamanho(&w));
}

// AdminMessage{ field: <submessage already assembled> } in 'out'. 0 = does not fit
// in the buffer OR passes what one packet takes (ADM_MAX) - in both cases the
// section cannot go, and whoever calls refuses the entire recording.
static size_t admin_msg(uint8_t* out, size_t size, int field, const pb_escritor_t* body)
{
  pb_escritor_t w = pb_escritor(out, size);
  pb_w_sub(&w, field, body);
  const size_t n = pb_tamanho(&w);
  return n <= ADM_MAX ? n : 0;
}

// THE GROSS SAVED IN THE DUMP, as a start of a section to be resent. The fields
// changes go AFTER it: nanopb stays with the last occurrence of each
// field, then the new value wins over the old one without having to rewrite the section.
// nullptr = ready to receive the fields; otherwise, WHY not (to the log).
static const char* comecar_of_bruto(pb_escritor_t* w, const bruto_t& b)
{
  taskENTER_CRITICAL(&s_mux);
  const bool tem = b.tem;
  if (tem) { pb_w_cru(w, b.b, b.n); }
  taskEXIT_CRITICAL(&s_mux);
  if (!tem)   { return "not present in the dump"; }
  if (!w->ok) { return "too large for one packet"; }
  return nullptr;
}

// The PRIMARY CHANNEL with different precision: the Channel is saved field by field, raw,
// minus settings.module_settings.position_precision, which goes with the value
// new. Here it is not necessary to just add it to the end, as in the sections: module_settings
// and SUBMESSAGE, and two occurrences of it do not guarantee the last one winning field
// a field - then the submessage is reassembled (its is_muted remains the same).
static bool montar_canal(pb_escritor_t* w, int32_t precisao)
{
  EXT_RAM_BSS_ATTR static bruto_t c;   // only the interface records
  taskENTER_CRITICAL(&s_mux);
  c = s_canal0;
  taskEXIT_CRITICAL(&s_mux);
  if (!c.tem) { return false; }
  pb_leitor_t l = pb_leitor(c.b, c.n);
  pb_field_t f;
  bool tem_settings = false;
  while (pb_field(&l, &f)) {
    if (!(f.num == 2 && f.type == PB_LEN)) { pb_w_cru(w, f.ini, f.n_total); continue; }
    tem_settings = true;
    uint8_t sb[160];
    pb_escritor_t ws = pb_escritor(sb, sizeof(sb));
    pb_leitor_t ls = pb_leitor(f.val, f.n_val);
    pb_field_t g;
    uint8_t mb[16];
    pb_escritor_t wm = pb_escritor(mb, sizeof(mb));
    pb_w_num(&wm, 1, (uint64_t)precisao);                       // position_precision
    while (pb_field(&ls, &g)) {
      if (g.num == 7 && g.type == PB_LEN) {                     // module_settings: the rest of it
        pb_leitor_t lm = pb_leitor(g.val, g.n_val);
        pb_field_t h;
        while (pb_field(&lm, &h)) { if (h.num != 1) { pb_w_cru(&wm, h.ini, h.n_total); } }
      } else {
        pb_w_cru(&ws, g.ini, g.n_total);
      }
    }
    pb_w_sub(&ws, 7, &wm);
    if (!ws.ok || !wm.ok) { return false; }
    pb_w_sub(w, 2, &ws);
  }
  return tem_settings && w->ok;
}

const char* mesh_cfg_reason(void) { return s_reason; }

bool mesh_cfg_gravar(const mesh_cfg_mudanca_t* m)
{
  s_reason[0] = 0;
  if (estado() != MESH_CFG_PRONTO) {
    ESP_LOGW(TAG, "save rejected: no node or incomplete dump");
    snprintf(s_reason, sizeof(s_reason), "no node, or configuration is still arriving");
    return false;
  }

  // ASSEMBLE EVERYTHING BEFORE SENDING ANYTHING. If a section cannot go
  // (unknown, prohibited, too big), nothing exit - half transaction
  // recorded would be a configuration that no one asked for.
  // device, position, lora, bluetooth, mqtt, telemetry + network + channel
  // primary + owner = 9.
  enum { MAX_MSGS = 10 };
  EXT_RAM_BSS_ATTR static uint8_t msgs[MAX_MSGS][ADM_MAX + 16];   // only the interface records
  size_t size[MAX_MSGS];
  int n_msgs = 0;

  bool feito[MESH_CFG_N] = { };
  int msg_mqtt = -1;   // index, in msgs[], of MQTT section (see the rule of Bluetooth below)
  for (int i = 0; i < MESH_CFG_N; ++i) {
    if (!m->muda[i] || feito[i]) { continue; }
    const field_def_t& c = CAMPOS[i];
    if (secao_proibida(c.modulo, c.secao)) { ESP_LOGE(TAG, "secao proibida"); return false; }

    // The section is saved + ALL its changed fields, at the end.
    EXT_RAM_BSS_ATTR static uint8_t body[ADM_MAX];
    pb_escritor_t wc = pb_escritor(body, sizeof(body));
    if (const char* porque = comecar_of_bruto(&wc, c.modulo ? s_mod[c.secao] : s_cfg[c.secao])) {
      ESP_LOGW(TAG, "section %s %d %s - cannot save", c.modulo ? "module" : "config", c.secao, porque);
      return false;
    }
    for (int k = i; k < MESH_CFG_N; ++k) {
      if (!m->muda[k] || CAMPOS[k].modulo != c.modulo || CAMPOS[k].secao != c.secao) { continue; }
      pb_w_int32(&wc, CAMPOS[k].num, m->v[k]);
      feito[k] = true;
    }

    // Config{ section: body } or ModuleConfig{ section: body }, inside of
    // AdminMessage{ set_config | set_module_config }.
    EXT_RAM_BSS_ATTR static uint8_t sec[ADM_MAX];
    pb_escritor_t ws = pb_escritor(sec, sizeof(sec));
    pb_w_sub(&ws, c.secao, &wc);
    if (n_msgs >= MAX_MSGS) { return false; }
    size[n_msgs] = admin_msg(msgs[n_msgs], sizeof(msgs[0]), c.modulo ? ADM_SET_MODULE_CONFIG : ADM_SET_CONFIG, &ws);
    if (!size[n_msgs]) {
      ESP_LOGE(TAG, "secao grande demais para um pacote");
      snprintf(s_reason, sizeof(s_reason), "configuração grande demais para um pacote");
      return false;
    }
    if (c.modulo && c.secao == 1) { msg_mqtt = n_msgs; }
    ++n_msgs;
  }

  // NO WI-FI: Config.network = saved section + wifi_enabled(1) + ssid(3)
  // + psk(4) at end. The password is not iteration of no, so it only goes with name AND password.
  if (m->wifi_ssid[0] || m->wifi_psk[0] || m->wifi_muda) {
    const bool tem_rede = m->wifi_ssid[0] || m->wifi_psk[0];
    if (tem_rede && (!m->wifi_ssid[0] || (!m->wifi_psk[0] && !m->wifi_open))) { ESP_LOGW(TAG, "wifi: precisa de nome e senha juntos"); return false; }
    EXT_RAM_BSS_ATTR static uint8_t body[ADM_MAX];
    pb_escritor_t wc = pb_escritor(body, sizeof(body));
    if (const char* porque = comecar_of_bruto(&wc, s_cfg[4])) { ESP_LOGW(TAG, "wifi: a secao network %s - nada enviado", porque); return false; }
    pb_w_num(&wc, 1, m->wifi_liga ? 1 : 0);                    // wifi_enabled
    if (tem_rede) {
      pb_w_text(&wc, 3, m->wifi_ssid, 32);                    // wifi_ssid
      pb_w_text(&wc, 4, m->wifi_psk, 64);                     // wifi_psk
    }
    EXT_RAM_BSS_ATTR static uint8_t sec[ADM_MAX];
    pb_escritor_t ws = pb_escritor(sec, sizeof(sec));
    pb_w_sub(&ws, 4, &wc);                                     // Config.network = 4
    if (n_msgs >= MAX_MSGS) { return false; }
    size[n_msgs] = admin_msg(msgs[n_msgs], sizeof(msgs[0]), ADM_SET_CONFIG, &ws);
    if (!size[n_msgs]) { ESP_LOGE(TAG, "wifi: secao grande demais para um pacote"); return false; }
    ++n_msgs;
  }

  // LOCATION IN THE MESH: set_channel of primary with new precision.
  if (m->precisao_muda) {
    EXT_RAM_BSS_ATTR static uint8_t body[ADM_MAX];
    pb_escritor_t wc = pb_escritor(body, sizeof(body));
    if (!montar_canal(&wc, m->precisao)) {
      ESP_LOGW(TAG, "primary channel: not in the dump or too large - nothing sent");
      snprintf(s_reason, sizeof(s_reason), "the node's primary channel has not arrived yet");
      return false;
    }
    if (n_msgs >= MAX_MSGS) { return false; }
    size[n_msgs] = admin_msg(msgs[n_msgs], sizeof(msgs[0]), ADM_SET_CHANNEL, &wc);
    if (!size[n_msgs]) { ESP_LOGE(TAG, "channel too large for one packet"); return false; }
    ++n_msgs;
  }

  // NAMES: the User stored in our no + the names in the end. Resend the User
  // integer preserves what set_owner also reads (is_licensed,
  // is_unmessagable) - with just the names, someone in the ham radio would lose their license.
  if (m->longo[0] || m->curto[0]) {
    EXT_RAM_BSS_ATTR static uint8_t u[ADM_MAX];
    pb_escritor_t wu = pb_escritor(u, sizeof(u));
    // Without the User of it won't: just with the names, set_owner would turn off the
    // is_licensed of one on ham radio (it compares the field, missing = false).
    if (const char* porque = comecar_of_bruto(&wu, s_user)) { ESP_LOGW(TAG, "nomes: o User do nosso no %s - nada enviado", porque); return false; }
    if (m->longo[0]) { pb_w_text(&wu, 2, m->longo, MESH_NOME_LONGO - 1); }   // long_name
    if (m->curto[0]) { pb_w_text(&wu, 3, m->curto, MESH_NOME_CURTO - 1); }   // short_name
    if (n_msgs >= MAX_MSGS) { return false; }
    size[n_msgs] = admin_msg(msgs[n_msgs], sizeof(msgs[0]), ADM_SET_OWNER, &wu);
    if (!size[n_msgs]) { ESP_LOGE(TAG, "nome grande demais para um pacote"); return false; }
    ++n_msgs;
  }

  if (n_msgs == 0) { return false; }

  // THE MQTT SECTION BY BLUETOOTH GOES ALONE, WITHOUT TRANSACTION. The firmware
  // (AdminModule::handleSetModuleConfig, case mqtt) calls disableBluetooth()
  // SO YOU receive the MQTT section - even with the open transaction. Inside
  // begin/commit, BLE crashed before of commit: the transaction was open (the
  // firmware does not expire), nothing was recorded and it did not restart, now without
  // Bluetooth until someone unplugs it.
  // Outside of transaction the same set_module_config validates, writes to disk and
  // restarts no alone (saveChanges with shouldReboot) - BLE crashes soon
  // then it doesn't interfere with anything. Therefore, via Bluetooth, the MQTT section
  // needs to be the ONLY change: any other change after it would not arrive.
  // There is no Bluetooth connection via the cable, and the normal transaction is valid.
  if (msg_mqtt >= 0 && tab5_mesh_via() == SETTINGS_MESH_TRANSPORT_BLUETOOTH) {
    if (n_msgs > 1) {
      ESP_LOGW(TAG, "pelo Bluetooth a secao MQTT grava sozinha - outras mudancas pendentes junto");
      snprintf(s_reason, sizeof(s_reason), "pelo Bluetooth, MQTT/proxy grava sozinho: grave o resto antes");
      return false;
    }
    if (!tab5_mesh_interno_admin(msgs[msg_mqtt], size[msg_mqtt])) {
      snprintf(s_reason, sizeof(s_reason), "the link to the node rejected the send");
      return false;
    }
    ESP_LOGI(TAG, "MQTT section sent without a transaction (Bluetooth) - node saves, disables BLE, and reboots");
    s_gravando = true;
    s_gravando_ms = tab5_now_ms();
    ++s_versao;
    tab5_mesh_interno_reapresentar_em(15000, true);
    return true;
  }

  // begin -> sections -> commit. The commit writes and resets the no.
  bool ok = admin_varint(ADM_BEGIN_EDIT, 1);
  for (int i = 0; ok && i < n_msgs; ++i) { ok = tab5_mesh_interno_admin(msgs[i], size[i]); }
  if (ok) { ok = admin_varint(ADM_COMMIT_EDIT, 1); }
  if (!ok) {
    // Transaction open without commit. The firmware DOES NOT expire: the changes remain
    // so in RAM of no, without recording, until it restarts.
    ESP_LOGE(TAG, "link rejected the transaction midway - the node will not save until rebooted");
    snprintf(s_reason, sizeof(s_reason), "the link dropped during saving - reboot the node");
    return false;
  }
  ESP_LOGI(TAG, "transaction sent (%d message(s)) - node saves and reboots", n_msgs);
  s_gravando = true;
  s_gravando_ms = tab5_now_ms();
  ++s_versao;
  // By serial the restarted node does not speak to itself: request the configuration later
  // of boot (BLE reconnects and requests it alone; request of new does not hurt).
  tab5_mesh_interno_reapresentar_em(15000, true);
  return true;
}

bool mesh_cfg_position_fixa_of_gps(void)
{
  if (estado() != MESH_CFG_PRONTO) { return false; }
  const nmea_position_t g = tab5_gps_position();
  if (!g.valid || g.qualidade == 0) { ESP_LOGW(TAG, "posicao fixa: GPS do Tab5 sem fix"); return false; }
  // The same Position that the service sends to the mesh (mesh_pb_posicao).
  uint8_t pos[32];
  pb_escritor_t wp = pb_escritor(pos, sizeof(pos));
  mesh_pb_position(&wp, (int32_t)(g.lat * 1e7), (int32_t)(g.lon * 1e7), (int32_t)g.altitude_m,
                  tab5_time_epoch());
  uint8_t a[48];
  const size_t n = admin_msg(a, sizeof(a), ADM_SET_FIXED_POSITION, &wp);
  const bool ok = n && tab5_mesh_interno_admin(a, n);
  // The position section in the saved dump still says "not fixed": it asks for new.
  if (ok) { ESP_LOGI(TAG, "posicao fixa enviada (%.5f, %.5f)", g.lat, g.lon); tab5_mesh_interno_reapresentar_em(3000, false); }
  return ok;
}

bool mesh_cfg_tirar_position_fixa(void)
{
  if (estado() != MESH_CFG_PRONTO) { return false; }
  const bool ok = admin_varint(ADM_REMOVE_FIXED_POS, 1);
  if (ok) { tab5_mesh_interno_reapresentar_em(3000, false); }
  return ok;
}

bool mesh_cfg_acertar_hora(void)
{
  if (estado() != MESH_CFG_PRONTO) { return false; }
  const uint32_t now = tab5_time_epoch();
  if (!now) { ESP_LOGW(TAG, "the Tab5 clock is not set yet"); return false; }
  uint8_t a[8];
  pb_escritor_t w = pb_escritor(a, sizeof(a));
  pb_w_fixo32(&w, ADM_SET_TIME_ONLY, now);
  return tab5_mesh_interno_admin(a, pb_tamanho(&w));
}

bool mesh_cfg_reiniciar(void)
{
  if (estado() != MESH_CFG_PRONTO) { return false; }
  const bool ok = admin_varint(ADM_REBOOT_SECONDS, 5);
  if (ok) {
    s_gravando = true;   // same wait: no some and iteration
    s_gravando_ms = tab5_now_ms();
    ++s_versao;
    tab5_mesh_interno_reapresentar_em(15000, true);
  }
  return ok;
}

bool mesh_admin_gravando(void) { return s_gravando; }
