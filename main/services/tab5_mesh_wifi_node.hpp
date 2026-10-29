// NO MESHTASTIC WI-FI NETWORKS, saved on the card.
//
// No only has ONE network configured at a time (Config.network), and switch required
// enter name and password of new. Here are the networks that have already been recorded on it,
// to choose from a list. File: Meshtastic/wifi_no.txt, pair of lines by
// network (name, password encrypted with the device key - the same as wifi.txt of
// Tab5; "-" = network open). The C6L only sees 2.4 GHz, like the Tab5.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MESH_WIFI_REDES_MAX 8

typedef struct {
  char ssid[33];
  char senha[65];     // pure text, only in RAM; empty = open network
  bool open;
} mesh_wifi_rede_t;

void     mesh_wifi_node_load(void);                    // card -> RAM, in service
int      mesh_wifi_no_n(void);
bool     mesh_wifi_no_rede(int i, mesh_wifi_rede_t* o);  // 0 = the most recent
// Save (or update) and move to the top of the list. Empty password = open network.
bool     mesh_wifi_no_salvar(const char* ssid, const char* senha);
bool     mesh_wifi_no_esquecer(int i);
// Copies the networks saved in Tab5 that are not yet in the list.
// Returns how many were entered, or -1 if Tab5 does not have none loaded.
int      mesh_wifi_no_trazer_of_tab5(void);
uint32_t mesh_wifi_no_versao(void);
