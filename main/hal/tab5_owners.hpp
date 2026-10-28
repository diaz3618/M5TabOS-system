// OWNER OF EACH TASK - who is responsible for each running task, in the spirit
// of the "user / system" split in task managers.
//
// FreeRTOS knows only a task's NAME. Inferring its owner from that name would
// create a table that goes stale with every new task. Instead, the creator
// DECLARES the owner when it creates the task:
//   - SERVICE: system services (Wi-Fi, ESP-NOW, Mesh, GPS, logging...) register
//     their own tasks;
//   - APP: a task created by an open app (task_app_create, called from the UI
//     while that app is in front); the kernel registers it;
//   - BACKGROUND: an app's background work (kernel/background); the kernel
//     registers it;
//   - SYSTEM: everything else, and the default: kernel, drivers, and IDF itself.
//
// This lives in hal/ and depends only on FreeRTOS, so HAL, services, and the
// kernel can register tasks without depending on a higher layer. owner points
// to permanent text: a literal, or the app/background name, also a literal.
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum {
  OWNER_SYSTEM = 0,
  OWNER_SERVICE,
  OWNER_APP,
  OWNER_BACKGROUND,
} owner_type_t;

// Registers (or replaces) the owner. Call immediately after creating the task.
void tab5_owner_register(TaskHandle_t h, owner_type_t type, const char* owner);

// Removes a task that is about to die (its handle may later be reused). Call
// BEFORE vTaskDelete.
void tab5_owner_forget(TaskHandle_t h);

// Returns the registered owner, or OWNER_SYSTEM (and owner = NULL) if none.
owner_type_t tab5_owner_of(TaskHandle_t h, const char** owner);
