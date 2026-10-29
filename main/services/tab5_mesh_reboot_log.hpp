// NO MESHTASTIC RESET DIARY.
//
// The no does not count, according to the protocol, WHY it restarted (the "reset reason" of
// ESP32 only exists in its console log). What the Tab5 achieves and take note, in
// time when he notices the restart, everything HE was doing in iteration: if
// changed the 5V of Grove, reapplied the energy profile, which had sent
// on last, the voltage of battery. With this of to separate "the Tab5
// caused" of "the didn't fall alone" - deduction, not certainty.
//
// As you can see: MyNodeInfo.reboot_count (stays in NVS of no) has risen since
// last reading. It persists on the card (Meshtastic/reinicios.txt), one per line.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MESH_DIARIO_MAX 40

typedef enum {
  MESH_REI_BASE = 0,      // first reading of counter: starting point, not restart
  MESH_REI_SOZINHO,       // the counter went up with the Tab5 connected, without our request
  MESH_REI_PEDIDO,        // Tab5 ordered to restart / save configuration
  MESH_REI_AUSENTE,       // rose while Tab5 was not connected: exact time unknown
} mesh_rei_origem_t;

typedef enum { MESH_ENV_NENHUM = 0, MESH_ENV_PACOTE, MESH_ENV_CONFIG, MESH_ENV_HEARTBEAT, MESH_ENV_ADMIN } mesh_env_tipo_t;

typedef struct {
  uint32_t epoch;        // time (UTC) of record; 0 = Tab5 still didn't know the time
  uint32_t up_tab5_s;    // uptime of Tab5 in registry
  uint32_t contador;     // MyNodeInfo.reboot_count after restart
  uint32_t up_no_s;      // uptime of no in last telemetry BEFORE falling; 0 = unknown
  uint8_t  origem;       // mesh_rei_origem_t
  uint8_t  send_state_tipo;   // our last send_state that was not beat (mesh_env_tipo_t)
  uint16_t centivolts;   // voltage of battery bus at the moment (0 = no reading)
  int32_t  ma;           // bus current
  int32_t  send_state_s;      // how long before registration was this send_state; -1 = never
  int32_t  grove_s;      // as soon as Tab5 changed the 5V of Grove; -1 = never
  int32_t  profile_s;     // as soon as Tab5 reapplied the energy profile; -1 = never
} mesh_reinicio_t;

// ---- calls of service (tab5_mesh.cpp) ----------------------------------
void mesh_diario_carregar(void);                 // of card, in ascent of service
void mesh_diario_nota_send_state(uint8_t tipo);       // each ToRadio that exits
// Photo of context, when no notices that it has restarted (FromRadio.rebooted).
void mesh_diario_foto(uint32_t up_no_s);
// MyNodeInfo arrived with 'counter'; decide whether to become a record.
void mesh_diario_contador(uint32_t contador, uint32_t up_no_s, bool conectados_antes, bool request_nosso);

// ---- reading / screen --------------------------------------------------------
int      mesh_diario_read(mesh_reinicio_t* saida, int max);   // of youngest on oldest
uint32_t mesh_diario_versao(void);
void     mesh_diario_limpar(void);                           // erases RAM and card
// Short sentence with the strongest indication for this register.
const char* mesh_diario_veredito(const mesh_reinicio_t* r);
