// Framework for an app's tasks, with ORDERLY SHUTDOWN.
//
// The PROBLEM it solves: killing a task from outside with vTaskDelete()
// kills it wherever it is - including INSIDE a driver, holding one of its
// internal semaphores. And FreeRTOS has no way to revoke ownership: a dead
// task's mutex stays taken forever, with the owner pointer pointing at
// garbage. There is no "force the release."
//
// The ONLY correct path is not to kill it: ask it to exit and let the task
// itself return what it borrowed. What makes this fast is xTaskAbortDelay(),
// which pulls the task out of a locked wait RIGHT AWAY - it returns from
// uart_read_bytes, from xQueueReceive, from vTaskDelay, as if the deadline
// had expired. Without it, you'd have to wait out each wait's natural timeout.
//
// HOW TO USE IT:
//
//   static app_task_t s_t;
//
//   static void my_loop(app_task_t* t) {
//       while (app_task_active(t)) {
//           ...work...                      // with LIMITED waits
//       }
//       // return whatever you borrowed here: fclose, xSemaphoreGive, etc.
//   }
//
//   on_open():   app_task_create(&s_t, my_loop, "gps", 4096, 3);
//   on_close():  app_task_stop(&s_t, 200);   // only then destroy queues
//
// When the loop RETURNS, the framework notifies whoever is waiting and the
// task-pool slot (kernel/task_pool.h) goes back to sleep - the app NEVER
// calls vTaskDelete (that would kill the whole slot). And stop() only
// returns once the task has confirmed it exited.

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "task_pool.h"

typedef struct app_task_s app_task_t;

typedef void (*app_task_body_t)(app_task_t* t);

struct app_task_s {
  TaskHandle_t       handle;   // nullptr = no task (the slot may belong to a later run)
  pool_id_t          id;       // the run in the fixed task pool (kernel/task_pool.h)
  SemaphoreHandle_t  end;      // the task signals this once it has finished cleaning up
  volatile bool      stop;
  app_task_body_t body;
};

// Creates the task. Returns false if memory ran out (and nothing is left half-done).
bool app_task_create(app_task_t* t, app_task_body_t body,
                      const char* name, uint32_t stack, UBaseType_t priority);

// The task's loop asks this on every iteration.
static inline bool app_task_active(const app_task_t* t) { return !t->stop; }

// Requests exit, unblocks the task, and WAITS for it to confirm, up to ms.
// Once this function returns, it's safe to destroy queues, drivers, and buffers.
void app_task_stop(app_task_t* t, uint32_t ms);
