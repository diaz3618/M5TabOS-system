// COOPERATIVE STOPPING OF A SERVICE TASK.
//
// ------------------------------------------------------------------------
// WHY DOES THIS EXIST
// ------------------------------------------------------------------------
// Three services (GPS, Mesh, Bluetooth of Mesh link) stop the task itself
// of the same way - and each one wrote the new mechanism: a flag
// "asked to stop", another "the task exited", and whoever asks WAIT
// ASKING every 10 ms (for ... vTaskDelay(10)). Three copies with
// differences that no one chose: on the GPS the flags weren't even volatile
// (read of one task, written of another), and the end of each wait was delayed
// up to 10 ms after the task has already exited.
//
// Here the task DA one semaforo on exit and whoever asked WAIT ON IT: wakes up on
// instant of exit, without asking anything in the middle. The rule of safety of
// three remains the same, now written only once (parada_pedir).
//
// ------------------------------------------------------------------------
// WHY COOPERATIVE (and not vTaskDelete, nor xTaskAbortDelay)
// ------------------------------------------------------------------------
// The task takes mutex and writes it to the card; killing it in middle leaves the mutex
// arrested forever (FreeRTOS does not revoke possession). And xTaskAbortDelay, which
// the apps frame uses (app_task.h), it also does NOT work here: it takes away the
// task of ANY wait - including of one xSemaphoreTake(mutex,
// portMAX_DELAY) whose return these services do not match. The task would follow
// thinking he has the mutex, and the next Give, of who is not the owner, drops the
// device. Then the task exits ALONE, at its exit point, where it does not
// holds nothing; the wait of who requested and the maximum time of an iteration (the
// term of reading of UART, typically).
//
// ------------------------------------------------------------------------
// USE
// ------------------------------------------------------------------------
// static stop_t s_stop;
//
// start(): if (!parada_prepare(&s_parada)) { ...out of memory... }
// if (!pool_criar(...)) { para_desistir(&s_parada); ... }
// task: while (!parada_pedida(&s_parada)) { ...one iteration... }
// ...release what is the safe task...
// stop_confirm(&s_stop);   // the LAST thing
// stop(): if (!parada_pedir(&s_parada, 1000)) { return false; }
// ...so now disassemble UART, queues, mutex...

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
  SemaphoreHandle_t exited;     // given by the task on exit; whoever asked waits for him
  volatile bool     request;   // asked to stop - task reads every iteration
  volatile bool     alive;     // there is an execution that has not yet been collected
} stop_t;

// Before creating the task. The traffic light is born once and serves forever.
// false = out of memory (do not create the task).
static inline bool stop_prepare(stop_t* p)
{
  if (!p->exited) { p->exited = xSemaphoreCreateBinary(); }
  if (!p->exited) { return false; }
  xSemaphoreTake(p->exited, 0);   // leftover from a previous run: does not count towards this
  p->request = false;
  p->alive   = true;
  return true;
}

// The task was not created: undo the prepare.
static inline void stop_abandon(stop_t* p) { p->alive = false; p->request = false; }

// In task: true when and time of exit.
static inline bool stop_requested(const stop_t* p) { return p->request; }

// On task: the LAST thing before returning. From now on who asked
// can dismantle everything she wore.
static inline void stop_confirm(stop_t* p) { xSemaphoreGive(p->exited); }

// The task exists and has not yet confirmed its exit.
static inline bool stop_running(const stop_t* p)
{
  return p->alive && p->exited && uxSemaphoreGetCount(p->exited) == 0;
}

// Order placed and not yet collected (the "still stopping" of services).
static inline bool stop_em_progress(const stop_t* p) { return p->alive && p->request; }

// ASK AND WAIT until 'month'. 'poke' (optional) is called already and every 100 ms
// while waiting - for the task that sleeps on a PROPER wait (the link
// Bluetooth expects notification and response from stack BLE) and needs to be
// agreed to see the request.
//
// true = the task exited: it is safe to disassemble what it used. Also true if
// there was no task.
// false = did not confirm within the deadline. THE ORDER IS STANDING and NOTHING should be
// disassembled: the exit when task can, and the next call (which
// finds the traffic light already set) ends the service. Download the request here
// would open the run in which the task has already read "stop", exit, and no one
// most collects - orphaned UART and mutex with service saying "of pe".
static inline bool stop_request(stop_t* p, uint32_t ms, void (*cutucar)(void) = nullptr)
{
  if (!p->alive) { return true; }
  p->request = true;
  const uint32_t wait_slice_ms = cutucar ? 100 : ms;
  for (uint32_t esperou = 0; ; esperou += wait_slice_ms) {
    if (cutucar) { cutucar(); }
    if (xSemaphoreTake(p->exited, pdMS_TO_TICKS(wait_slice_ms)) == pdTRUE) {
      p->alive   = false;
      p->request = false;
      return true;
    }
    if (esperou + wait_slice_ms >= ms) { return false; }
  }
}
