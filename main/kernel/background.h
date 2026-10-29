// BACKGROUND PART OF AN APP - work that continues when the window closes
// .
//
// ------------------------------------------------------------------------
// THE PROBLEM
// ------------------------------------------------------------------------
// An app lives while it is on screen: when_closing() releases everything. But there is the app
// whose JOB is not the screen - record the track of GPS, send the position via
// internet, one long calculation. The Trail solved this by becoming a job done
// by hand (services/tab5_trilha, with its own task, notification and stop). O
// second app like this would have to repeat everything - and each one would repeat differently.
//
// ------------------------------------------------------------------------
// THE MODEL
// ------------------------------------------------------------------------
// The app is divided into two halves:
// - the WINDOW: the app of always (draw, touch, be born and die with the screen);
// - the BACKGROUND PART: one loop that the KERNEL runs in its task. The app only
// describes (fundo_def_t) and asks to enable/disable. Close the window
// to the bottom.
//
// What the SYSTEM guarantees at all times, without the app writing anything:
// - Own TASK, priority 1 (below the interface, radios and
// worker) - the heaviest loop never locks the touch;
// - ALWAYS VISIBLE: while running, a NOTIF_ESTADO notification with the name
// her (gray tablet in bar). Nothing runs hidden from the user;
// - ENERGY added: the declared profile enters the effective profile of the device
// (next to app of screen, or of home) while it runs - WiFi request
// here stays of pe with any app on screen and in sleep;
// - CLEAN STOP: request to stop WAKES UP the loop (fundo_expect iteration in
// time) and the system waits for it to exit before calling ao_parar();
// - who stops: the window, the console ("bottom stop <name>"), and - when
// exists - a screen of Running.
//
// ------------------------------------------------------------------------
// THE LOOP CONTRACT (body)
// ------------------------------------------------------------------------
// static void meu_laco(fundo_t* f) {
// while (fundo_wait(f, 5000)) { // sleep 5 s; false = stop
// ...work with LIMITED waits...
// state_fund(f, n, "42 submissions");    // the text of tablet
// }
// } // returned: system cleans
//
// - NEVER wait forever (portMAX_DELAY): the request of stop needs
// manage to arrive. Expect of truth and Fundo_expect().
// - Loop of CALCULO (without waiting anything) calls Fundo_ceder(f) at each iteration:
// without this the task occupies the entire core, its IDLE never runs, the
// watchdog goes off and the device doesn't sleep. fund_ceder so cede of fact
// every ~20 ms, so it can be called in a tight loop.
// - The loop DOES NOT draw. What the window needs to show, the background keeps in
// own state (protected) and the window reads by copy - equal service.
// - Memory of background and background: static or allocated at ao_initiar() and
// released in ao_parar(). NEVER one buffer of window - it can close a
// any time.
// - Return from loop alone (finished the work) and allowed: the system
// perceives and clears as in a requested stop.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tab5_power.hpp"
#include "notification.h"

#define FUNDO_MAX 4   // shares of background running on same team

typedef struct background_s background_t;
typedef void (*background_body_t)(background_t* f);

typedef struct {
  const char*   name;       // literal: name of task (<=15), of notification and in lists
  background_body_t body;
  uint32_t      stack;      // bytes; 0 = 4096
  tab5_profile_t profile;     // which needs to be ON while running (adds to the effective profile)
  int           sleep_clock_floor_mhz;   // 0 = the default of system
  // Options. They run in the task of those who requested it (the interface, in practice).
  bool (*on_start)(void); // prepares the state; false = refuse enable
  void (*on_stop)(void);   // after loop exited: close and release
  bool (*com_sound)(void);    // preference of beep of notification; nullptr = with sound
} background_def_t;

// ---- for window (and any one) ----------------------------------------
bool system_fundo_start(const background_def_t* d);   // false: already running, no vacancy or declined
bool system_fundo_stop(const background_def_t* d);     // WAIT for the exit loop (up to 3 s)
bool system_fundo_active(const background_def_t* d);

typedef struct {
  const char* name;
  uint32_t    ha_s;                  // running how many seconds ago
  char        summary[NOTIF_RESUMO];  // the last state_fund()
} background_info_t;
int  system_fundo_listar(background_info_t* out, int max);
bool system_fundo_stop_name(const char* name);

// ---- for loop ----------------------------------------------------------
bool background_wait(background_t* f, uint32_t ms);   // sleeps for months; false = asked to stop
bool background_yield(background_t* f);                  // loop of calculation; false = asked to stop
bool background_running(const background_t* f);
void background_estado(background_t* f, int count, const char* summary);

// ---- for the kernel --------------------------------------------------------
void background_somar_profile(tab5_profile_t& p);   // called bysistema_impor_preferencias()
int  background_piso_sleep(void);               // highest floor request (0 = none)
void background_manter(void);                     // in the loop of interface: clears whoever finished alone,
                                             // reapplies the profile when the set changes
