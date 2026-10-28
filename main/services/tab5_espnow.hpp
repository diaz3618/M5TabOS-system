// ESP-NOW - system service.
//
// The P4 has no radio, and esp_hosted does not carry ESP-NOW: esp_now_* does
// not even link on this side. ESP-NOW therefore runs INSIDE the C6, in our
// firmware (c6/), and this service communicates through esp_hosted's peer_data
// channel. The message contract lives in shared/tab5_c6.h.
//
// Like Mesh: the service owns the radio and history, publishes notifications
// while the app is closed, and the ESP-NOW app is only a window.
//
// It exists only with our C6 firmware (tab5_wifi_c6_is_ours). With factory
// firmware, the service waits silently and nothing breaks.
//
// CHANNEL: ESP-NOW works between devices on the SAME WiFi channel. When
// associated with a router, the C6 stays on its channel, so a device talking
// to Tab5 (Cardputer) must use that channel. tab5_espnow_canal() reports it.
//
// POWER: while sleeping between beacons (modem sleep), the C6 misses ESP-NOW
// frames. The service requests the receiver stay enabled only with a fixed
// channel, while the app is open and the screen is on, or when there has been
// traffic in the last 5 minutes. Otherwise the radio saves power like normal WiFi.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tab5_c6.h"

#define ESPNOW_HIST 64   // messages kept in RAM (the card keeps everything)

typedef enum {
  ESPNOW_ENVIO_NADA = 0,   // received
  ESPNOW_ENVIO_PENDENTE,   // sent by Tab5; the C6 has not confirmed yet
  ESPNOW_ENVIO_OK,         // the C6 transmitted (broadcast has no ACK: "left the antenna")
  ESPNOW_ENVIO_FALHOU,
} espnow_send_state_t;

typedef struct {
  uint8_t  mac[6];         // quem mandou (recebida) ou para quem (minha)
  bool     minha;
  bool     difusao;
  bool     espressif;       // arrived in the esp-now component format; false = plain text
  uint8_t  send_state;          // espnow_envio_t
  uint32_t when;         // UTC epoch; 0 = clock not set yet
  char     text[C6_TEXTO_MAX + 1];
} espnow_msg_t;

// Start the task. Call at boot, after WiFi.
void tab5_espnow_start(void);

// O C6 confirmou o ESP-NOW no ar (mandou o PRONTO).
bool    tab5_espnow_pronto(void);
uint8_t tab5_espnow_canal(void);
void    tab5_espnow_meu_mac(uint8_t out[6]);
bool    tab5_espnow_c6_compativel(void);   // o C6 tem o firmware nosso?

// RAM history: 0 is the oldest. Returns a copy (the table changes in another task).
int      tab5_espnow_n(void);
bool     tab5_espnow_msg(int i, espnow_msg_t* out);
uint32_t tab5_espnow_total(void);    // only increases - "changed since I looked"

// Distinct devices heard during this boot, and how long ago the last frame was.
int      tab5_espnow_vistos(void);
uint32_t tab5_espnow_ms_since_rx(void);   // UINT32_MAX = nothing yet

// CHANNEL. 0 = the router's channel (default: WiFi remains connected). 1..13
// = FIXED channel: WiFi releases the router (the radio can use only one channel
// at a time) and ESP-NOW stays there. This reaches devices off the router's
// channel (a standalone Cardputer stays on channel 1), or keeps ESP-NOW alive
// outside router range. Non-blocking.
bool    tab5_espnow_set_canal(uint8_t canal);
uint8_t tab5_espnow_canal_fixo(void);

// Broadcast (everyone on the channel hears it). Non-blocking: queues in the
// service. false = queue full, empty text, or ESP-NOW unavailable.
bool tab5_espnow_send(const char* text);

// The app reports when its window opens/closes. While open (and the screen is
// on), the receiver stays enabled. See POWER above.
void tab5_espnow_window(bool open);

// Unread (received since the last time someone looked).
int  tab5_espnow_nao_lidas(void);
void tab5_espnow_marcar_lidas(void);
