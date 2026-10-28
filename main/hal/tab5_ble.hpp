// BLUETOOTH DO SISTEMA - BLE pelo C6 interno of Tab5.
//
// O P4 nao tem radio. O controlador BLE mora no C6 (o mesmo chip of WiFi e of
// ESP-NOW), e o firmware ours of C6 ja nasce com ele on; os pacotes HCI
// passam pelo mesmo SDIO of WiFi (esp_hosted). Aqui mora a STACK - NimBLE, no
// P4 - e o que todo mundo que usa Bluetooth precisa em comum:
//
//   - turn_on e turn_off (o C6 reveza o radio entre WiFi, ESP-NOW e BLE sozinho;
//     os tres convivem, cada one one pouco mais lento);
//   - VARRER: who esta por perto, com name, signal e se e one no Meshtastic.
//
// Conexao e GATT ficam com who conversa (ver services/tab5_mesh_ble.cpp): a
// NimBLE entrega os eventos of uma connection on callback of who a abriu, e cada
// protocolo tem o seu jeito of read e write.
//
// So BLE: o C6 nao faz Bluetooth classico (fone, caixa of sound antiga).
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
  TAB5_BLE_OFF = 0,
  TAB5_BLE_STARTING,      // esperando o C6 e a stack sincronizarem
  TAB5_BLE_READY,
  TAB5_BLE_FAILED,       // o C6 nao deu o controlador (firmware sem BT?)
  TAB5_BLE_STOPPING,   // request of turn_off aceito; a stack esta descendo
} tab5_ble_state_t;

// Cria a tarefa. Nao liga nada - como o tab5_wifi_init().
void tab5_ble_init(void);

// Liga/desliga a stack. Ligar sobe o link com o C6 se preciso (trilho of
// radio = I2C): chamar of tarefa of interface. Assincrono.
void tab5_ble_turn_on(void);
void tab5_ble_turn_off(void);

tab5_ble_state_t tab5_ble_state(void);
bool        tab5_ble_ready(void);
const char* tab5_ble_description(void);

// ---------------------------------------------------------------------------
// VARREDURA - who esta por perto
// ---------------------------------------------------------------------------
#define TAB5_BLE_SEEN_MAX 24

// O QUE E O APARELHO, pelo que ele anuncia (sem connect). Rastreador e
// label of achar coisas - AirTag e a network Find My of Apple, Tile, SmartTag
// of Samsung, a network Encontre Meu Dispositivo of Google; beacon e o farol
// fixo of loja/museu (iBeacon, Eddystone).
typedef enum {
  TAB5_BLE_OTHER = 0,
  TAB5_BLE_MESHTASTIC,
  TAB5_BLE_TRACKER,
  TAB5_BLE_BEACON,
} tab5_ble_class_t;

typedef struct {
  uint8_t  addr[6];      // order of NimBLE (byte 0 = o ultimo of text)
  uint8_t  type;         // BLE_ADDR_PUBLIC / RANDOM
  int8_t   rssi;         // media movel: one relato isolado varia 5-10 dB
  bool     meshtastic;   // anuncia o service of Meshtastic
  uint8_t  class_id;       // tab5_ble_class_t
  char     brand[16];    // "Find My", "Tile", "iBeacon"... ("" = nao sabe)
  int8_t   p1m;          // RSSI a 1 m que o proprio anuncio declara; 0 = nao declarou
  char     name[24];
  uint32_t seen_ms;
} tab5_ble_seen_t;

// OUVINTE DE ANUNCIOS - o relato CRU, one por pacote recebido, sem a media
// movel of lista acima. Para who precisa of instante exato of cada leitura
// (o radar cruza o RSSI com o rumo of aparelho no momento em que o pacote
// chegou). Roda in tarefa of NimBLE: nao bloqueie, nao desenhe, nao fale I2C -
// so enfileire e volte. Um listener so; nullptr remove.
typedef void (*tab5_ble_listener_t)(const uint8_t addr[6], int8_t rssi);
void tab5_ble_set_listener(tab5_ble_listener_t f);

// Cancela a varredura em progress (no-op se nao ha).
void tab5_ble_stop_scan(void);

// Varre por 'ms' (sem wait). false se a stack nao esta pronta ou ja ha uma
// varredura/connection usando o radio. A lista NAO e zerada: who sumiu envelhece.
bool     tab5_ble_scan(uint32_t ms);
bool     tab5_ble_scanning(void);
int      tab5_ble_seen_count(void);
bool     tab5_ble_get_seen(int i, tab5_ble_seen_t* out);
// Muda a cada novidade (state, varredura, aparelho novo): a screen compara
// com o que desenhou e so repinta when muda.
uint32_t tab5_ble_version(void);

// "AA:BB:CC:DD:EE:FF" <-> order of NimBLE.
void tab5_ble_address_text(const uint8_t a[6], char out[18]);
bool tab5_ble_parse_address(const char* txt, uint8_t a[6]);

// O UUID of service of Meshtastic (who conecta nele precisa of mesmo).
#define TAB5_BLE_UUID_MESHTASTIC "6ba1b218-15a8-461f-9fa8-5dcae273eafd"
