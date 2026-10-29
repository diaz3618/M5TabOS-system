// GPS service - reading of UART and parse of NMEA on Grove, independent of
// which app is on screen.
//
// SAME MOLD of tab5_mesh: the KERNEL turns this service on and off (Grove
// hot-swap - see kernel/core.cpp), not the app. Before, reading
// GPS lived INSIDE of app Azimutal - UART installed in ao_abrir(),
// uninstalled in ao_close(). It worked, but only Azimutal saw the
// position: no other app had the ability to ask "where am I" (Mesh
// needed this for live localization, and it was the trigger of change).
//
// Now who reads the UART and parses RMC/GGA (time and position of
// RECEIVER) and this service. Azimutal continues to own what he only needs -
// the skyplot of constellation (GSV) is the history recorded on the card - and for
// this subscribes to receive the raw sentences (see tab5_gps_subscribe).
//
// GROVE POSSESSION: exactly the same arbitration as always
// (tab5_grove_acquire/release, GROVE_OWNER_GPS) - so now who takes and what
// service, no boot or in exchange of module, and no longer the app on open.

#pragma once

#include "nmea.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <stdint.h>

// Take the Grove (GROVE_OWNER_GPS) and upload the UART + the reading task. Call
// only with the Grove free_space and one GPS of truth detected - normally this is the
// kernel who decides this (see tab5_grove_detect() in kernel/core.cpp).
bool tab5_gps_start(void);

// Download the task, close the UART and return the Grove. false = failed
// stop a time (same semantics as tab5_mesh_parar) - nothing was
// disassembled in this case.
bool tab5_gps_stop(void);

bool tab5_gps_service_active(void);

// Last known position of RECEIVER (not the satellites - this is of
// Azimuth). 'valid' stays false until the first GGA arrives; 'quality' 0
// means "the receiver fails, but still no fix".
nmea_position_t tab5_gps_position(void);

// UTC date/time of own GPS, and if there is FIX (field A of RMC).
nmea_time_t tab5_gps_time(void);

// Is there a recent sentence? (same spirit as tab5_mesh_no_ar()).
bool tab5_gps_on_air(void);

// How many months have there been NO sentences - used by the watchman of Grove in
// kernel/core.cpp to know when the module disappeared from cable, in the same way as
// tab5_mesh_silencio_ms() was already done for the radio.
uint32_t tab5_gps_silence_ms(void);

// SIGNATURE OF RAW SENTENCES - for those who need something besides
// position/hour (today, only Azimuthal, for GSV/skyplot).
//
// A SOLE SUBSCRIBER, of purpose: and the same spirit of "who takes shall
// release" of Grove - simplicity over generality that no one asked for
// still. If more than one day is needed, the solution is to make a small list,
// Don't complicate this now without real use.
//
// The queue receives ALL valid sentences (checksum ok), including those that
// own service has already processed (RMC/GGA) - filter by prefix and work
// of who signs, as Azimutal already did before this change.
bool tab5_gps_subscribe(QueueHandle_t queue);
void tab5_gps_unsubscribe(void);
