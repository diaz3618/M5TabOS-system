// CSI - the WiFi channel state between the router and Tab5, as seen by the P4.
//
// WHAT IT SAYS. CSI is the amplitude and phase of each OFDM subcarrier in a
// received frame. With one antenna it does NOT provide direction, but changes
// significantly when something moves between the router and Tab5: a person
// walking, a door opening, even with no phone nearby. This service converts
// that signal into a MOVEMENT level (still / light / strong).
//
// WHERE IT COMES FROM. CSI is measured in the OFDM preamble; the router's
// 2.4 GHz beacon (1 Mbps, 11b) does not contain it. With no traffic to the C6,
// almost nothing arrives. The service therefore pings the router at 20 Hz
// while enabled (the same technique as Espressif's esp-csi) and asks the C6
// for CSI only from THE ROUTER. When sniffing is enabled, the C6 also measures
// unrelated frames, which are not suitable as a reference.
//
// THE DEVICE MUST NOT MOVE. Rotating or holding Tab5 changes CSI just as much
// as a person walking. The caller reports movement through
// tab5_csi_aparelho_mexendo(), and the service freezes the reference until the
// device has been still again.
//
// SINGLE OWNER OF THE MESSAGE. P4 peer_data stores ONE listener per msg_id,
// and registering a new one REPLACES the old one. This service therefore owns
// C6_MSG_CSI; callers that need raw records (such as the console test) use
// tab5_csi_ouvinte().
//
// IDLE COST: zero—no task, ping, or C6 request. When enabled: one pool slot,
// the 20 Hz ping, and the C6 receiver kept enabled.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tab5_c6.h"

// Requesters (one bit each, like tab5_wifi_keep_awake): the service stays
// enabled while at least one requester remains.
#define TAB5_CSI_RADAR    (1u << 0)
#define TAB5_CSI_CONSOLE  (1u << 1)

void tab5_csi_enable(uint32_t quem);
void tab5_csi_disable(uint32_t quem);
bool tab5_csi_active(void);

typedef enum {
  TAB5_CSI_DESLIGADO = 0,
  TAB5_CSI_SEM_C6,          // C6 without our firmware, or without recorded CSI
  TAB5_CSI_SEM_ROTEADOR,    // no WiFi connection: there is nothing to measure
  TAB5_CSI_APRENDENDO,      // medindo o "parado" deste lugar (primeiros segundos)
  TAB5_CSI_APARELHO_MEXENDO,
  TAB5_CSI_MEDINDO,
} tab5_csi_fase_t;

typedef enum { TAB5_CSI_PARADO = 0, TAB5_CSI_LEVE, TAB5_CSI_FORTE } tab5_csi_level_t;

typedef struct {
  tab5_csi_fase_t  fase;
  tab5_csi_level_t level;      // vale em TAB5_CSI_MEDINDO
  float            razao;      // current deviation / still deviation (1 = still)
  uint16_t         por_s;      // CSI records in the last second
  int8_t           rssi;       // most recent record
} tab5_csi_estado_t;

void tab5_csi_estado(tab5_csi_estado_t* out);

// Is Tab5 moving (rotation, handheld)? While true, measurement is suspended
// and the reference restarts when it stops. Cheap: only one variable is stored.
void tab5_csi_aparelho_mexendo(bool sim);

// Raw records, delivered from the esp_hosted event task: copy and return.
// Only one listener is supported (the most recent caller); nullptr removes it.
typedef void (*tab5_csi_ouvinte_t)(const c6_csi_cab_t* cab, const uint8_t* data, uint16_t n);
void tab5_csi_ouvinte(tab5_csi_ouvinte_t cb);
