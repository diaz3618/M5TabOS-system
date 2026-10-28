// SNIFFING - WiFi promiscuous mode, as seen by the P4.
//
// esp_hosted does not forward promiscuous mode, so it runs INSIDE the C6
// (our firmware, in c6/) and arrives here already SUMMARIZED: the C6 groups
// by MAC during each batch period and sends one batch (see SNIFFER in
// shared/tab5_c6.h). This service stores the latest state for every heard MAC;
// users (Radar, console) read the table when needed.
//
// IDLE COST: zero. There is no permanent task or table: on enable, it takes a
// pool slot (renews the C6 request) and allocates the table in PSRAM; on
// disable, it returns both.
//
// POWER: sniffing keeps the C6 receiver enabled continuously. The C6 therefore
// DISABLES ITSELF if this service stops renewing (the app closed or crashed),
// so callers must disable it when the screen using sniffing closes.
//
// CHANNELS: with WiFi connected, the C6 stays on the router's channel (switching
// would drop the connection). To scan several channels, release the router
// first (tab5_wifi_radio_only_mode), as fixed-channel ESP-NOW already does.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tab5_c6.h"

#define FAREJO_MAX 128   // Stored MACs; when full, the least recently heard is evicted

typedef struct {
  uint8_t  mac[6];
  int8_t   rssi_ult;      // of ultimo lote
  int8_t   rssi_max;      // of ultimo lote
  int8_t   rssi_med;      // of ultimo lote
  uint8_t  tipos;         // C6_FT_*, acumulado since que apareceu
  uint8_t  channel;
  uint32_t frames;        // acumulado
  uint32_t seen_ms;       // tab5_now_ms() of ultimo lote em que apareceu
} farejo_item_t;

// Enable or replace the configuration (c6_farejo_cfg_t in tab5_c6.h; cfg->liga
// is ignored). Non-blocking. false = C6 lacks our firmware, or no slot exists.
bool tab5_farejo_enable(const c6_farejo_cfg_t* cfg);
void tab5_farejo_disable(void);
bool tab5_farejo_active(void);

// Does the C6 firmware support sniffing? Read from PRONTO/INFO; false until known.
bool tab5_farejo_c6_suporta(void);
// All capabilities reported by the C6 (C6_CAP_*), from PRONTO/INFO; 0 until known.
uint8_t tab5_farejo_caps_c6(void);

// EACH RECORD FROM EACH BATCH, for users that need RSSI over time (Radar pairs
// each one with the device bearing at that instant). Called from the esp_hosted
// event task: copy and return. 't_us' is the MIDDLE of the batch period
// (esp_timer), the best estimate of when that average RSSI was measured.
// One listener only; nullptr removes it.
typedef void (*tab5_farejo_ouvinte_t)(const c6_farejo_reg_t* reg, int64_t t_us);
void tab5_farejo_ouvinte(tab5_farejo_ouvinte_t cb);

// Table. Copy returned because batches arrive in another task. Arrival order.
int      tab5_farejo_n(void);
bool     tab5_farejo_item(int i, farejo_item_t* out);
bool     tab5_farejo_achar(const uint8_t mac[6], farejo_item_t* out);
uint32_t tab5_farejo_lotes(void);      // only increases: "a new batch arrived since I looked"
uint32_t tab5_farejo_perdidos(void);   // MACs that did not fit in the C6 table, accumulated
void     tab5_farejo_limpar(void);

// Query the C6 (contract version, heap, frames). BLOCKS for up to 'espera_ms'
// - only from a task that may wait (console), never the interface.
bool tab5_farejo_info_c6(c6_info_t* out, uint32_t espera_ms);

// Called by the ESP-NOW service when PRONTO arrives (version and capabilities).
void tab5_farejo_anotar_pronto(const uint8_t* data, uint16_t n);
