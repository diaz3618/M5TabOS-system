// Energy logging - a SYSTEM SERVICE, not an application.
//
// WHY THIS IS NOT AN APP: applications are created when opened and destroyed
// when closed. If logging lived in the Energy app, data would exist only while
// that app was on screen, but the question it answers ("when did the battery
// run out?") requires CONTINUOUS logging, including with the screen off and
// another app open. It is therefore a system service that starts at boot and
// never stops. It has no task of its own; it rides on the "card" task
// (tab5_sd_carona), which already wakes once per second.
//
// HOW IT SAVES POWER: a mounted card consumes power. We therefore keep one
// sample per minute in RAM and FLUSH to the card only every 10 samples,
// mounting, appending, and unmounting it. The card stays disabled for more
// than 99% of the time, without losing samples.
//
// Arquivo: /sdcard/apps/Sistema/energia.csv
//   ENE,<t_s>,<V>,<mA>,<mW>,<pct>,<MHz>,<sleep>,<estado>,<app>
//
// -------------------------------------------------------------------------
// SECOND FILE, IN THE SAME WINDOW: TASK AND HEAP HEALTH
// -------------------------------------------------------------------------
// The requirement is a record that shows, over time, how much stack headroom
// each task has and how the heap is changing. This makes it possible to spot
// a steadily growing allocation before it becomes a Guru Meditation. It is
// the same reasoning as energy.csv: a curve is more useful than one isolated
// number, applied to memory.
//
// It is written in the SAME flush as energia.csv, using the same card mount,
// interval, and existing task. The marginal cost is only opening one more
// file while the card is already mounted.
//
// Arquivo: /sdcard/apps/Sistema/tarefas.csv
//   TASKS,<t_s>,<n_tarefas>,<heap_int_livre>,<heap_int_minimo>,<psram_livre>,<psram_minimo>
//   TASK,<t_s>,<name>,<pilha_livre_bytes>,<core>,<priority>   (uma por tarefa)
//
// pilha_livre_bytes is the HIGH WATER MARK: the smallest amount of headroom
// that stack has ever had, not its current headroom. It only decreases, so it
// warns that a task is getting close before a real overflow occurs.

#pragma once

#include <stdint.h>

// Start logging on the "card" task. Call after M5.begin() and tab5_sd_init().
void registro_start(void);

// Name of the foreground app, so the system can include it in the log.
// Pass NULL when the home screen is active.
void registro_app_current(const char* name);

// Number of samples waiting in RAM, and number already written to the card.
// Used by the Energy app to show that logging is alive.
void registro_estado(int* in_ram, uint32_t* gravadas, uint32_t* falhas);
