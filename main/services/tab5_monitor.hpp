// SYSTEM MONITOR - a service that measures the device ALL THE TIME.
//
// Previously, CPU and memory measurement existed only while the Tasks app was
// open: the sampling task lived and died with it, so anything that happened
// while it was closed (a spike, memory falling overnight) went unseen.
//
// Now a service task (priority 1, below everything except IDLE) samples every
// second without drawing anything, and stores:
//   - the current snapshot: CPU per task, free stack, and memory;
//   - PEAKS with their timestamps: highest CPU per task and for the system,
//     lowest free RAM/PSRAM, smallest largest block, and lowest stack;
//   - five minutes of history (CPU, RAM, PSRAM) for the graph;
//   - EVENTS triggered by small thresholds (see tab5_monitor.cpp): memory
//     below a limit, a new minimum, a sharp drop, fragmentation, nearly full
//     stack, a task stuck at high CPU, and a new or vanished task.
//
// Card records, in apps/Tarefas/:
//   eventos-AAAA-MM-DD.csv  - one event per line, when it happens;
//   resumo-AAAA-MM-DD.csv   - one line per MINUTE: worst CPU, lowest RAM,
//                             lowest PSRAM, smallest largest block—how much
//                             memory remained throughout the day.
// Every event also goes to the log (tag "monitor"), which tab5_log records.
//
// The Tasks app is only a window: it reads from here and measures nothing.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"

// This used to be 32, and uxTaskGetSystemState() returns ZERO when there are
// more tasks than slots, without explaining why. Wi-Fi + Bluetooth + Mesh push
// the system past 32 (esp_hosted alone creates seven), so samples were dropped
// and Tasks displayed the last good snapshot: numbers detached from reality.
#define MON_MAX_TAREFAS 64
#define MON_HIST_N      300     // 5 min a 1 sample/s
#define MON_EVENTOS_N   32
#define MON_NOME        16

typedef struct {
  char        name[MON_NOME];
  UBaseType_t numero;         // xTaskNumber - identidade estavel
  UBaseType_t priority;
  BaseType_t  core;           // tskNO_AFFINITY = any
  float       pct;            // CPU in the last second (both cores)
  float       pct_pico;       // highest seen since boot (or reset)
  uint32_t    pct_pico_em;    // seconds of uptime at the peak
  uint32_t    stack_livre;    // bytes - the smallest remaining headroom (high water mark)
  uint8_t     type;           // owner_type_t: system, service, app, background
  char        dono[MON_NOME]; // owner ("WiFi", "Camera", "Trilha"); empty = system
} mon_tarefa_t;

typedef struct {
  uint32_t livre, total;
  uint32_t maior_bloco;       // largest allocation possible NOW (fragmentation)
  uint32_t min_livre;         // lowest free amount seen by the monitor
  uint32_t min_livre_em;      // uptime em s
  uint32_t min_bloco;         // lowest largest-block value seen
  uint32_t min_bloco_em;
} mon_memory_t;

typedef struct {
  uint32_t     uptime_s;
  float        cpu;           // uso total (100 - IDLE)
  float        cpu_pico;
  uint32_t     cpu_pico_em;
  mon_memory_t    ram;           // interna
  mon_memory_t    psram;
  uint32_t     foto_us;       // duration of the last uxTaskGetSystemState (critical section)
  uint32_t     foto_pior_us;  // worst since boot - grows with stack outside L2MEM
  int          n;
  mon_tarefa_t t[MON_MAX_TAREFAS];   // ordenadas por xTaskNumber
} mon_foto_t;

typedef struct {
  uint32_t uptime_s;
  uint32_t epoch;             // 0 = clock not set yet
  char     text[72];
} mon_evento_t;

// Start the task. Call at boot, after SD and logging.
void tab5_monitor_start(void);

// Copy the current snapshot. false = the monitor has no first sample yet.
bool tab5_monitor_foto(mon_foto_t* out);

// History, oldest to newest. Returns the number copied.
int tab5_monitor_historico(float* cpu, uint32_t* ram_livre, uint32_t* psram_livre, int max);

// Events, newest to oldest. Returns the number copied.
int tab5_monitor_eventos(mon_evento_t* out, int max);
uint32_t tab5_monitor_total_eventos(void);   // contador que so sobe

// Restart peak tracking (including memory minima).
void tab5_monitor_reset_picos(void);

// INTERNAL RAM LEDGER - what consumed how much.
//
// The monitor sees RAM fall but not WHY: "80 KB free" does not say whether it
// was the panel, C6 SDIO, NimBLE, or a new task. Each marker records internal
// free RAM and the difference from the previous marker, like a bank statement.
// Markers are placed at boot (main.cpp, sistema_iniciar) and when each
// radio/service starts; the console's "memory" command prints the table, and
// every marker also goes to the log (tag "memory").
//
// May be called from any task at any time, including before
// tab5_monitor_start(). Allocates nothing. Do not call from an ISR.
#define MEM_MARCOS_N 40

typedef struct {
  uint32_t ms;            // uptime in ms
  uint32_t livre;         // free internal RAM (bytes)
  uint32_t maior_bloco;   // largest free internal block
  uint32_t minimum;        // lowest internal RAM seen so far: below
                          // 'livre' = a temporary peak WITHIN the stage (missing from delta)
  int32_t  delta;         // free - free at previous marker (negative = consumed)
  char     etapa[28];
} memory_marco_t;

void tab5_memory_marco(const char* etapa);

// Copy markers, oldest to newest. Returns the number copied.
int tab5_memory_extrato(memory_marco_t* out, int max);
