#include "app_task.h"
#include "tab5_owners.hpp"
#include "app.h"          // system_app_current
#include "internal.hpp"   // system_in_interface
#include "esp_log.h"

static const char *TAG = "app_task";

// Wraps the app's loop: when it returns, it notifies whoever is waiting and
// only then does the task delete itself. The app never calls vTaskDelete.
//
// 'end' CAPTURED HERE, BEFORE the body runs - NOT read from 't->end' later.
// A real bug, hit on hardware with the camera: a 720x1280 photo took more
// than 10s to save to the card (well beyond any timeout app_task_stop()
// in this system would use), so the task was "abandoned" - and
// app_task_stop()'s own comment already correctly warned that abandoning
// does NOT delete the semaphore (only the CALLER's 't->end' becomes
// nullptr, so the caller stops touching it). The problem is that this
// trampoline also sees the SAME 't' (the same pointer passed to
// xTaskCreatePinnedToCore) - so, when it finally finished, it read
// 't->end' AGAIN, found nullptr, and xSemaphoreGive(nullptr) brought down
// the whole system (assert in xQueueGenericSend, null "pxQueue").
//
// By keeping the handle HERE, on this task's own stack, before the body
// starts, the trampoline always gives the REAL semaphore (it still exists -
// only 't->end' was zeroed, not the object), whether 't' was abandoned or
// not. If nobody is waiting for it anymore (the abandoned case), the give
// just marks the semaphore as "given" and nobody ever reads it on the next
// iteration - a controlled leak, exactly the trade-off app_task_stop()
// already accepted in writing ("leaking is bad; corrupting is worse"), just
// without the crash now.
static void trampoline(void* arg)
{
  app_task_t* t = (app_task_t*)arg;
  SemaphoreHandle_t end = t->end;
  t->body(t);
  xSemaphoreGive(end);
  // Returns: the pool slot gives up its owner and goes back to sleep. Never vTaskDelete.
}

bool app_task_create(app_task_t* t, app_task_body_t body,
                      const char* name, uint32_t stack, UBaseType_t priority)
{
  t->handle = nullptr;
  t->stop  = false;
  t->body  = body;

  // Binary, not counting: only a single "I'm done" matters to us.
  t->end = xSemaphoreCreateBinary();
  if (!t->end) { return false; }

  // AUTOMATIC OWNER (hal/tab5_owners.hpp): created by the interface with an
  // app in front = that app's task (the Camera opens "cam", Radar opens
  // "NMEAParse"). Whoever creates one outside of that (a service, at boot)
  // registers its own owner - and a later registration swaps the owner, so
  // the service always wins. The owner travels with the request: the slot
  // registers it BEFORE the function starts.
  const char* app = (system_app_current() && system_in_interface()) ? system_app_current() : nullptr;
  if (!pool_create(&t->id, trampoline, t, name, stack, priority,
                  app ? OWNER_APP : OWNER_SYSTEM, app)) {
    vSemaphoreDelete(t->end);
    t->end = nullptr;
    return false;
  }
  t->handle = t->id.handle;
  return true;
}

void app_task_stop(app_task_t* t, uint32_t ms)
{
  if (!t->handle) { return; }

  t->stop = true;

  // Pulls the task out of a block RIGHT NOW, without waiting out its
  // timeout. Returns pdFAIL if it wasn't blocked - in that case it'll see
  // the flag on its next iteration anyway, so there's nothing more to do.
  pool_wake(&t->id);

  if (xSemaphoreTake(t->end, pdMS_TO_TICKS(ms)) == pdTRUE) {
    vSemaphoreDelete(t->end);
    t->end = nullptr;
    t->handle = nullptr;
    t->id = (pool_id_t)POOL_ID_EMPTY;
    return;
  }

  // Didn't confirm in time. We do NOT kill the task and do NOT destroy the
  // semaphore: it may still be alive and deliver later, and freeing the
  // semaphore now would crash. Leaking a task and a semaphore is bad;
  // corrupting a driver or writing to freed memory is worse. The log exists
  // so this doesn't go unnoticed.
  ESP_LOGE(TAG, "task did not confirm exit within %lums - abandoned on purpose",
           (unsigned long)ms);
  t->end = nullptr;
  t->handle = nullptr;
  t->id = (pool_id_t)POOL_ID_EMPTY;
}
