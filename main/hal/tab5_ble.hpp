// SYSTEM BLUETOOTH - BLE provided by the Tab5's internal C6.
//
// The P4 has no radio. The BLE controller lives on the C6, the same chip that
// provides Wi-Fi and ESP-NOW. Our C6 firmware enables it by default, and HCI
// packets use the same Wi-Fi SDIO link (esp_hosted). This header owns the
// common NimBLE stack on the P4 and provides the following system services:
//
//   - turn_on and turn_off; the C6 shares its radio among Wi-Fi, ESP-NOW, and
//     BLE, so all three coexist with some performance cost;
//   - scanning for nearby devices, their names, signal strength, and
//     Meshtastic status.
//
// Connections and GATT belong to the protocol that uses them (see
// services/tab5_mesh_ble.cpp). NimBLE delivers connection events through the
// callback registered by the opener, and each protocol handles its own reads
// and writes.
//
// BLE only: the C6 does not provide classic Bluetooth audio or peripherals.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
  TAB5_BLE_OFF = 0,
  TAB5_BLE_STARTING,      // waiting for the C6 and stack to synchronize
  TAB5_BLE_READY,
  TAB5_BLE_FAILED,        // the C6 did not provide a controller
  TAB5_BLE_STOPPING,      // turn_off accepted; the stack is stopping
} tab5_ble_state_t;

// Initializes the service. Does not enable anything, like tab5_wifi_init().
void tab5_ble_init(void);

// Enables/disables the stack. Enabling starts the C6 link if needed; the radio
// rail is controlled over I2C, so these functions belong in the interface task.
// The operation is asynchronous.
void tab5_ble_turn_on(void);
void tab5_ble_turn_off(void);

tab5_ble_state_t tab5_ble_state(void);
bool        tab5_ble_ready(void);
const char* tab5_ble_description(void);

// ---------------------------------------------------------------------------
// SCANNING - nearby devices
// ---------------------------------------------------------------------------
#define TAB5_BLE_SEEN_MAX 24

// Classify a device from its advertisement, without connecting. Trackers help
// find objects (Apple AirTag/Find My, Tile, Samsung SmartTag, and Google Find
// My Device); beacons are fixed transmitters such as iBeacon or Eddystone.
typedef enum {
  TAB5_BLE_OTHER = 0,
  TAB5_BLE_MESHTASTIC,
  TAB5_BLE_TRACKER,
  TAB5_BLE_BEACON,
} tab5_ble_class_t;

typedef struct {
  uint8_t  addr[6];      // NimBLE order; byte 0 is last in the text form
  uint8_t  type;         // BLE_ADDR_PUBLIC / RANDOM
  int8_t   rssi;         // moving average; one report can vary by 5-10 dB
  bool     meshtastic;   // advertises the Meshtastic service
  uint8_t  class_id;     // tab5_ble_class_t
  char     brand[16];    // "Find My", "Tile", "iBeacon"... (empty = unknown)
  int8_t   p1m;          // advertised RSSI at 1 m; 0 means not advertised
  char     name[24];
  uint32_t seen_ms;
} tab5_ble_seen_t;

// ADVERTISEMENT LISTENER - one raw callback per received packet, without the
// list's moving average. Use this when the exact timestamp of each reading is
// needed. It runs in the NimBLE task: do not block, draw, or use I2C; enqueue
// the data and return. Only one listener is supported; nullptr removes it.
typedef void (*tab5_ble_listener_t)(const uint8_t addr[6], int8_t rssi);
void tab5_ble_set_listener(tab5_ble_listener_t f);

// Cancels an active scan (no-op if none is active).
void tab5_ble_stop_scan(void);

// Scans for 'ms' without waiting. Returns false if the stack is not ready or
// another scan/connection is using the radio. The list is not cleared; missing
// devices simply age out.
bool     tab5_ble_scan(uint32_t ms);
bool     tab5_ble_scanning(void);
int      tab5_ble_seen_count(void);
bool     tab5_ble_get_seen(int i, tab5_ble_seen_t* out);

// Increments on every change (state, scan, or new device), allowing the screen
// to repaint only when its displayed data is stale.
uint32_t tab5_ble_version(void);

// Convert between "AA:BB:CC:DD:EE:FF" and NimBLE byte order.
void tab5_ble_address_text(const uint8_t a[6], char out[18]);
bool tab5_ble_parse_address(const char* txt, uint8_t a[6]);

// Meshtastic service UUID; clients must connect to this UUID.
#define TAB5_BLE_UUID_MESHTASTIC "6ba1b218-15a8-461f-9fa8-5dcae273eafd"
