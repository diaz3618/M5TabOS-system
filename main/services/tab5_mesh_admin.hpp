// CONFIGURATION OF NO MESHTASTIC FROM TAB5 - without PC or cell phone.
//
// Speaks the AdminMessage of Meshtastic itself (ADMIN_APP port, 6), the same as the
// web client and mobile app use. Details of protocol at docs/MESHTASTIC.md.
//
// HOW A CHANGE TRAVELS
// 1. The configuration dump (FromRadio 5 and 9) brings each entire SECTION; the
// raw bytes of each are stored here (mesh_admin_save_*).
// 2. set_config REPLACES the entire section in no. Just send the changed field
// would reset the others - then the saved section with the new fields goes
// ADDED at end. In protobuf, repeated scalar field is worth the last
// occurrence (this is how the nanopb of firmware decodes).
// 3. All in one transaction: begin_edit_settings, sections, set_owner,
// commit_edit_settings. No records once and restarts itself; the service
// asks for the configuration of new and the screen shows the values that NO
// confirmed - never the ones we asked for.
//
// SECURITY
// - Only with the other side (cable or Bluetooth) and with the complete dump.
// There is no admin on the internet.
// - LOCAL Admin (without 'from') dispenses with the session key: AdminModule.cpp,
// of 2.5 to current. The key is only valid for administering REMOTELY.
// - The Wi-Fi of no (network) is only resent when the user turns on/off the
// Wi-Fi or choose/enter a network (name AND password: the saved password does not have
// how to read of iteration, so there is no "change just the name"). So wifi_enabled,
// ssid and psk change; The rest of the section follows as instructed.
// - Sections that this module never resends: network without the two fields above,
// power (the dump replaces ls_secs with the default), security (keys) and the module
// serial (and the Grove link itself).
// - The PRIMARY channel is only resent (set_channel) when the need for
// position changes; name, key, uplink and the rest follow as instructed.
// - AdminMessage over 233 bytes (the ceiling of Data.payload) and refused here.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "tab5_mesh.hpp"

// What to change. Each one is ONE field of ONE section (see FIELDS in .cpp).
typedef enum {
  MESH_CFG_PAPEL = 0,      // Config.device.role
  MESH_CFG_NODEINFO_S,     // Config.device.node_info_broadcast_secs
  MESH_CFG_POS_S,          // Config.position.position_broadcast_secs
  MESH_CFG_SALTOS,         // Config.lora.hop_limit
  MESH_CFG_TX_DBM,         // Config.lora.tx_power (0 = maximum legal of region)
  MESH_CFG_BT,             // Config.bluetooth.enabled
  MESH_CFG_MQTT,           // ModuleConfig.mqtt.enabled
  MESH_CFG_TELE_S,         // ModuleConfig.telemetry.device_update_interval
  MESH_CFG_MQTT_PROXY,     // ModuleConfig.mqtt.proxy_to_client_enabled (Tab5 lends the internet)
  MESH_CFG_POS_INTELIGENTE,// Config.position.position_broadcast_smart_enabled (send on moves)
  MESH_CFG_GPS_MODO,       // Config.position.gps_mode: 0 disabled, 1 enabled, 2 without GPS on board
  MESH_CFG_N
} mesh_cfg_field_t;

typedef enum {
  MESH_CFG_SEM_NO = 0,     // no link to node, or dump has not finished yet
  MESH_CFG_PRONTO,         // of for read and write
  MESH_CFG_GRAVANDO,       // transaction sent; waiting for it to restart and reintroduce itself
} mesh_cfg_estado_t;

typedef struct {
  uint8_t  state;                  // mesh_cfg_state_t
  bool     conhecido[MESH_CFG_N];  // the field section came in the dump
  int32_t  v[MESH_CFG_N];          // value (proto3: missing = 0)

  // LOCATION IN THE MESH: ChannelSettings.module_settings.position_precision
  // of PRIMARY channel - and she turns the position on and off in Meshtastic
  // (0 = no position none; 32 = exact; 10..19 = only the region,
  // with fewer bits). -1 = the channel did not appear in the dump. Edited by
  // mesh_cfg_mudanca_t.precision.
  int32_t  precisao;

  // Read only.
  bool     pos_fixa;               // Config.position.fixed_position
  int32_t  regiao;                 // Config.lora.region (-1 = did not come)
  int32_t  preset;                 // Config.lora.modem_preset
  bool     usa_preset;
  char     firmware[24];
  uint8_t  hw;                     // DeviceMetadata.hw_model
  bool     tem_wifi, tem_bt;
  uint32_t reinicios;              // MyNodeInfo.reboot_count
  int32_t  nos_no_radio;           // MyNodeInfo.nodedb_count; -1 = old firmware does not send
  bool     wifi_conhecido;         // the network section came in the dump
  bool     wifi_enabled;            // Config.network.wifi_enabled
  char     wifi_ssid[33];          // Config.network.wifi_ssid (password is never read)
  char     aviso[96];              // last ClientNotification of no
  uint32_t notice_ms;               // uptime our when arrived; 0 = none
} mesh_cfg_t;

// Copies the current state (values confirmed by no).
void mesh_cfg_read(mesh_cfg_t* saida);

// Changes with each new piece of information (dump, recording, warning): the screen repaints.
uint32_t mesh_cfg_versao(void);

// A transaction with EVERYTHING changed. changes[i] = true takes v[i]; empty names
// remain as they are. The no records and restarts. false = nothing sent (no no, no
// dump, or unknown section) - and the reason goes to the log.
typedef struct {
  bool    muda[MESH_CFG_N];
  int32_t v[MESH_CFG_N];
  char    longo[MESH_NOME_LONGO];
  char    curto[MESH_NOME_CURTO];
  char    wifi_ssid[33];           // BOTH together or neither; turn on Wi-Fi
  char    wifi_psk[65];
  bool    wifi_open;             // network without password: psk goes empty
  bool    wifi_muda;               // change wifi_enabled (with or without new network)
  bool    wifi_liga;               // value of wifi_enabled when wifi_muda
  bool    precisao_muda;           // set_channel on primary channel with new precision
  int32_t precisao;                // see mesh_cfg_t.precision
} mesh_cfg_mudanca_t;
bool mesh_cfg_gravar(const mesh_cfg_mudanca_t* m);
// Why didn't the last mesh_cfg_gravar() send anything, short, to the screen
// ("" = sent, or the reason is only in the log).
const char* mesh_cfg_reason(void);

// Actions that do not restart the no (except the last one).
bool mesh_cfg_position_fixa_of_gps(void);   // set_fixed_position with GPS of Tab5
bool mesh_cfg_tirar_position_fixa(void);    // remove_fixed_position
bool mesh_cfg_acertar_hora(void);          // set_time_only with time of Tab5
bool mesh_cfg_reiniciar(void);             // reboot_seconds
bool mesh_admin_gravando(void);            // Tab5 told me to record/restart and it still hasn't come back

// TO SEND THE GPS POSITION OF THE TAB5 (tab5_mesh.cpp), without blocking the caller:
// - no one already knows where he is alone (fixed position, or his GPS enabled):
// that Tab5 sent would be ignored (fixed) or run over (his GPS);
// - the accuracy of primary channel (-1 = not known), for internet mode
// respect the same choice that does not respect.
bool    mesh_admin_no_tem_position_propria(void);
int32_t mesh_admin_precisao_primario(void);

// ---- hooks of service (tab5_mesh.cpp), with its lock taken ----------
void mesh_admin_guardar_secao(bool modulo, int secao, const uint8_t* bytes, size_t n);
void mesh_admin_guardar_user(const uint8_t* bytes, size_t n);         // User of OUR on
void mesh_admin_guardar_metadata(const uint8_t* bytes, size_t n);     // FromRadio 13
void mesh_admin_guardar_my_info(const uint8_t* bytes, size_t n);      // FromRadio 3
void mesh_admin_guardar_aviso(const uint8_t* bytes, size_t n);        // FromRadio 16
void mesh_admin_guardar_canal(int indice, const uint8_t* bytes, size_t n);   // FromRadio 10 (Entire Channel)
void mesh_admin_start_dump(void);   // want_config exited: whatever comes is the new truth
void mesh_admin_end_dump(void);      // config_complete_id arrived
void mesh_admin_tique(uint32_t now_ms);   // at each iteration of task of service
