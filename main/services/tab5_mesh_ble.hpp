// LINK BLE WITH NO MESHTASTIC.
//
// The other way, beyond Cape Grove. The protocol is the SAME - ToRadio and
// FromRadio in protobuf - only changes the transport:
//
// serial (Grove): each message goes in a frame 0x94 0xC3 <size> <body>
// BLE: each message is ONE characteristic value, without frame
// toRadio (written) - what Tab5 sends
// fromRadio (reading) - one message per read; empty = finished
// fromNum (notifies) - "there's something new on fromRadio"
//
// That's why the Mesh service (tab5_mesh.cpp) doesn't know about BLE: it delivers the body
// of ToRadio and receives the body of FromRadio, and whoever frames (serial) or writes
// in characteristic (here) and the link.
//
// With the C6L on Bluetooth the Grove stays free - and the GPS can go on it, with the
// position going to the mesh (settings_mesh_position_interval_min()).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Takes up the linking task: searches for the node (the address saved in settings, or the
// first Meshtastic that appears), connects, pairs with PIN and stays
// delivering what arrives. Reconnects by itself if dropped. The caller has already called
// Bluetooth (tab5_ble_turn_on, task of interface).
bool mesh_ble_start(void);
// Stops and disconnects. Wait for the exit task (up to 6 s).
bool mesh_ble_stop(void);
// The link task exists and has not yet exited (which will bring down the BLE stack
// asks before: it cannot disappear under a connection in progress).
bool mesh_ble_running(void);

// Enqueues one ToRadio (body, without 0x94 0xC3). false no connection.
bool mesh_ble_send(const uint8_t* body, size_t n);
// Next FromRadio that arrived (body), waiting until 'wait_months'. 0 = nothing.
size_t mesh_ble_receber(uint8_t* buf, size_t max, uint32_t espera_ms);
// true ONCE per new connection: and the cue to request the configuration on no.
bool mesh_ble_conectou_now(void);

bool        mesh_ble_conectado(void);
const char* mesh_ble_descricao(void);
// The name and address of no connection ("" no connection).
void        mesh_ble_no(char name[24], char endereco[18]);

// PIN ON TIME: the party did not ask for the PIN and the link is waiting for someone to enter it (the
// not with random PIN shows a new one with each pairing). Settings > Meshtastic
// opens the keyboard alone; mesh_ble_digitar_pin delivers (takes ~30 s).
bool mesh_ble_esperando_pin(void);
bool mesh_ble_digitar_pin(uint32_t pin);
// After one PIN is rejected, the link stops and waits: this starts again (the
// next pairing asks for PIN immediately).
void mesh_ble_tentar_of_novo(void);
