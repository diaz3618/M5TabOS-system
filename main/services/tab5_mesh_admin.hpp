// CONFIGURACAO DO NO MESHTASTIC A PARTIR DO TAB5 - sem PC nem celular.
//
// Fala o AdminMessage of proprio Meshtastic (porta ADMIN_APP, 6), o mesmo que o
// cliente web e o app of celular usam. Detalhes of protocolo em MESHTASTIC.md.
//
// COMO UMA MUDANCA VIAJA
//   1. O dump of configuracao (FromRadio 5 e 9) traz cada SECAO inteira; os
//      bytes crus of cada uma ficam guardados aqui (mesh_admin_guardar_*).
//   2. set_config SUBSTITUI a secao inteira no no. Mandar so o field alterado
//      zeraria os outros - entao vai a secao guardada com os campos novos
//      ACRESCENTADOS no end. Em protobuf, field escalar repetido vale a ultima
//      ocorrencia (e assim que o nanopb of firmware decodifica).
//   3. Tudo numa transacao: begin_edit_settings, as secoes, set_owner,
//      commit_edit_settings. O no grava uma vez e reinicia sozinho; o servico
//      pede a configuracao of novo e a screen mostra os valores que o NO
//      confirmou - nunca os que pedimos.
//
// SEGURANCA
//   - So com o no of outro lado (cabo ou Bluetooth) e com o dump complete.
//     Pela internet nao ha admin.
//   - Admin LOCAL (sem 'from') dispensa a chave of sessao: AdminModule.cpp,
//     of 2.5 ate a current. A chave so vale para administrar no REMOTO.
//   - O Wi-Fi of no (network) so e reenviado when o user liga/desliga o
//     Wi-Fi ou escolhe/digita uma rede (name E senha: a senha guardada nao tem
//     como ser lida of iteration, entao nao ha "mudar so o name"). So wifi_enabled,
//     ssid e psk mudam; o resto of secao segue como o no mandou.
//   - Secoes que este modulo nunca reenvia: network sem os dois campos acima,
//     power (o dump troca ls_secs pelo default), security (chaves) e o modulo
//     serial (e o proprio enlace of Grove).
//   - O canal PRIMARIO so e reenviado (set_channel) when a precisao of
//     position muda; name, chave, uplink e o resto seguem como o no mandou.
//   - AdminMessage acima of 233 bytes (o teto of Data.payload) e recusado aqui.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "tab5_mesh.hpp"

// O que of para mudar. Cada one e UM field of UMA secao (ver CAMPOS no .cpp).
typedef enum {
  MESH_CFG_PAPEL = 0,      // Config.device.role
  MESH_CFG_NODEINFO_S,     // Config.device.node_info_broadcast_secs
  MESH_CFG_POS_S,          // Config.position.position_broadcast_secs
  MESH_CFG_SALTOS,         // Config.lora.hop_limit
  MESH_CFG_TX_DBM,         // Config.lora.tx_power (0 = maximum legal of regiao)
  MESH_CFG_BT,             // Config.bluetooth.enabled
  MESH_CFG_MQTT,           // ModuleConfig.mqtt.enabled
  MESH_CFG_TELE_S,         // ModuleConfig.telemetry.device_update_interval
  MESH_CFG_MQTT_PROXY,     // ModuleConfig.mqtt.proxy_to_client_enabled (o Tab5 empresta a internet)
  MESH_CFG_POS_INTELIGENTE,// Config.position.position_broadcast_smart_enabled (manda on se move)
  MESH_CFG_GPS_MODO,       // Config.position.gps_mode: 0 disabled, 1 enabled, 2 sem GPS in placa
  MESH_CFG_N
} mesh_cfg_field_t;

typedef enum {
  MESH_CFG_SEM_NO = 0,     // sem enlace com o no, ou dump ainda nao terminou
  MESH_CFG_PRONTO,         // of para read e gravar
  MESH_CFG_GRAVANDO,       // transacao enviada; esperando o no reiniciar e se reapresentar
} mesh_cfg_estado_t;

typedef struct {
  uint8_t  state;                  // mesh_cfg_estado_t
  bool     conhecido[MESH_CFG_N];  // a secao of field veio no dump
  int32_t  v[MESH_CFG_N];          // valor (proto3: ausente = 0)

  // LOCALIZACAO NA MALHA: ChannelSettings.module_settings.position_precision
  // of canal PRIMARIO - e ela que liga e desliga a position no Meshtastic
  // (0 = o no nao manda position none; 32 = exata; 10..19 = so a regiao,
  // com bits a menos). -1 = o canal nao veio no dump. Editada por
  // mesh_cfg_mudanca_t.precisao.
  int32_t  precisao;

  // So leitura.
  bool     pos_fixa;               // Config.position.fixed_position
  int32_t  regiao;                 // Config.lora.region (-1 = nao veio)
  int32_t  preset;                 // Config.lora.modem_preset
  bool     usa_preset;
  char     firmware[24];
  uint8_t  hw;                     // DeviceMetadata.hw_model
  bool     tem_wifi, tem_bt;
  uint32_t reinicios;              // MyNodeInfo.reboot_count
  int32_t  nos_no_radio;           // MyNodeInfo.nodedb_count; -1 = firmware antigo nao manda
  bool     wifi_conhecido;         // a secao network veio no dump
  bool     wifi_enabled;            // Config.network.wifi_enabled
  char     wifi_ssid[33];          // Config.network.wifi_ssid (a senha nunca e lida)
  char     aviso[96];              // ultimo ClientNotification of no
  uint32_t notice_ms;               // uptime nosso when chegou; 0 = nenhum
} mesh_cfg_t;

// Copia o estado current (valores confirmados pelo no).
void mesh_cfg_read(mesh_cfg_t* saida);

// Muda a cada informacao nova (dump, gravacao, aviso): a screen repinta.
uint32_t mesh_cfg_versao(void);

// Uma transacao com TUDO o que changed. muda[i] = true leva v[i]; nomes vazios
// ficam como estao. O no grava e reinicia. false = nada enviado (sem no, sem
// dump, ou secao desconhecida) - e o reason vai para o log.
typedef struct {
  bool    muda[MESH_CFG_N];
  int32_t v[MESH_CFG_N];
  char    longo[MESH_NOME_LONGO];
  char    curto[MESH_NOME_CURTO];
  char    wifi_ssid[33];           // os DOIS juntos ou nenhum; liga o Wi-Fi of no
  char    wifi_psk[65];
  bool    wifi_open;             // rede sem senha: o psk vai vazio
  bool    wifi_muda;               // mexer em wifi_enabled (com ou sem rede nova)
  bool    wifi_liga;               // valor of wifi_enabled when wifi_muda
  bool    precisao_muda;           // set_channel no canal primario com a precisao nova
  int32_t precisao;                // ver mesh_cfg_t.precisao
} mesh_cfg_mudanca_t;
bool mesh_cfg_gravar(const mesh_cfg_mudanca_t* m);
// Por que o ultimo mesh_cfg_gravar() nao mandou nada, curto, para a screen
// ("" = mandou, ou o reason esta so no log).
const char* mesh_cfg_reason(void);

// Acoes que nao reiniciam o no (exceto a ultima).
bool mesh_cfg_position_fixa_of_gps(void);   // set_fixed_position com o GPS of Tab5
bool mesh_cfg_tirar_position_fixa(void);    // remove_fixed_position
bool mesh_cfg_acertar_hora(void);          // set_time_only com a hora of Tab5
bool mesh_cfg_reiniciar(void);             // reboot_seconds
bool mesh_admin_gravando(void);            // o Tab5 mandou gravar/reiniciar e o no ainda nao voltou

// PARA O ENVIO DA POSICAO DO GPS DO TAB5 (tab5_mesh.cpp), sem trava of quem chama:
//  - o no ja sabe onde esta sozinho (position fixa, ou o GPS dele enabled): o
//    que o Tab5 mandasse seria ignorado (fixa) ou atropelado (GPS dele);
//  - a precisao of canal primario (-1 = nao sabida), para o modo internet
//    respeitar a mesma escolha que o no respeita.
bool    mesh_admin_no_tem_position_propria(void);
int32_t mesh_admin_precisao_primario(void);

// ---- ganchos of servico (tab5_mesh.cpp), com a trava dele tomada ----------
void mesh_admin_guardar_secao(bool modulo, int secao, const uint8_t* bytes, size_t n);
void mesh_admin_guardar_user(const uint8_t* bytes, size_t n);         // o User of NOSSO no
void mesh_admin_guardar_metadata(const uint8_t* bytes, size_t n);     // FromRadio 13
void mesh_admin_guardar_my_info(const uint8_t* bytes, size_t n);      // FromRadio 3
void mesh_admin_guardar_aviso(const uint8_t* bytes, size_t n);        // FromRadio 16
void mesh_admin_guardar_canal(int indice, const uint8_t* bytes, size_t n);   // FromRadio 10 (Channel inteiro)
void mesh_admin_start_dump(void);   // want_config exited: o que vier e a verdade nova
void mesh_admin_end_dump(void);      // config_complete_id chegou
void mesh_admin_tique(uint32_t now_ms);   // a cada iteration of tarefa of servico
