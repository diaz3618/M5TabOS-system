// BLUETOOTH DO SISTEMA - BLE pelo C6 interno do Tab5.
//
// O P4 nao tem radio. O controlador BLE mora no C6 (o mesmo chip do WiFi e do
// ESP-NOW), e o firmware ours do C6 ja nasce com ele on; os pacotes HCI
// passam pelo mesmo SDIO do WiFi (esp_hosted). Aqui mora a STACK - NimBLE, no
// P4 - e o que todo mundo que usa Bluetooth precisa em comum:
//
//   - turn_on e turn_off (o C6 reveza o radio entre WiFi, ESP-NOW e BLE sozinho;
//     os tres convivem, cada um um pouco mais lento);
//   - VARRER: who esta por perto, com name, signal e se e um no Meshtastic.
//
// Conexao e GATT ficam com who conversa (ver services/tab5_mesh_ble.cpp): a
// NimBLE entrega os eventos de uma conexao ao callback de who a abriu, e cada
// protocolo tem o seu jeito de read e escrever.
//
// So BLE: o C6 nao faz Bluetooth classico (fone, caixa de som antiga).
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
  TAB5_BLE_OFF = 0,
  TAB5_BLE_STARTING,      // esperando o C6 e a stack sincronizarem
  TAB5_BLE_READY,
  TAB5_BLE_FAILED,       // o C6 nao deu o controlador (firmware sem BT?)
  TAB5_BLE_STOPPING,   // pedido de turn_off aceito; a stack esta descendo
} tab5_ble_state_t;

// Cria a tarefa. Nao liga nada - como o tab5_wifi_init().
void tab5_ble_init(void);

// Liga/desliga a stack. Ligar sobe o link com o C6 se preciso (trilho do
// radio = I2C): chamar da tarefa de interface. Assincrono.
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
// label de achar coisas - AirTag e a network Find My da Apple, Tile, SmartTag
// da Samsung, a network Encontre Meu Dispositivo do Google; beacon e o farol
// fixo de loja/museu (iBeacon, Eddystone).
typedef enum {
  TAB5_BLE_OTHER = 0,
  TAB5_BLE_MESHTASTIC,
  TAB5_BLE_TRACKER,
  TAB5_BLE_BEACON,
} tab5_ble_class_t;

typedef struct {
  uint8_t  addr[6];      // ordem da NimBLE (byte 0 = o ultimo do text)
  uint8_t  type;         // BLE_ADDR_PUBLIC / RANDOM
  int8_t   rssi;         // media movel: um relato isolado varia 5-10 dB
  bool     meshtastic;   // anuncia o service do Meshtastic
  uint8_t  class_id;       // tab5_ble_class_t
  char     brand[16];    // "Find My", "Tile", "iBeacon"... ("" = nao sabe)
  int8_t   p1m;          // RSSI a 1 m que o proprio anuncio declara; 0 = nao declarou
  char     name[24];
  uint32_t seen_ms;
} tab5_ble_seen_t;

// OUVINTE DE ANUNCIOS - o relato CRU, um por pacote recebido, sem a media
// movel da lista acima. Para who precisa do instante exato de cada leitura
// (o radar cruza o RSSI com o rumo do aparelho no momento em que o pacote
// chegou). Roda na tarefa da NimBLE: nao bloqueie, nao desenhe, nao fale I2C -
// so enfileire e volte. Um listener so; nullptr remove.
typedef void (*tab5_ble_listener_t)(const uint8_t addr[6], int8_t rssi);
void tab5_ble_set_listener(tab5_ble_listener_t f);

// Cancela a varredura em curso (no-op se nao ha).
void tab5_ble_stop_scan(void);

// Varre por 'ms' (sem esperar). false se a stack nao esta pronta ou ja ha uma
// varredura/conexao usando o radio. A lista NAO e zerada: who sumiu envelhece.
bool     tab5_ble_scan(uint32_t ms);
bool     tab5_ble_scanning(void);
int      tab5_ble_seen_count(void);
bool     tab5_ble_get_seen(int i, tab5_ble_seen_t* out);
// Muda a cada novidade (state, varredura, aparelho novo): a tela compara
// com o que desenhou e so repinta quando muda.
uint32_t tab5_ble_version(void);

// "AA:BB:CC:DD:EE:FF" <-> ordem da NimBLE.
void tab5_ble_address_text(const uint8_t a[6], char out[18]);
bool tab5_ble_parse_address(const char* txt, uint8_t a[6]);

// O UUID do service do Meshtastic (who conecta nele precisa do mesmo).
#define TAB5_BLE_UUID_MESHTASTIC "6ba1b218-15a8-461f-9fa8-5dcae273eafd"
