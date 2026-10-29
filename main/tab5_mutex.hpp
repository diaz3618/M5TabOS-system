// LAZY LOCK - the mutex is created on the FIRST call to tab5_mutex_travar(),
// rather than in an "_iniciar()" function that every task would need to
// remember to call before using the module.
//
// ------------------------------------------------------------------------
// WHY THIS EXISTS
// ------------------------------------------------------------------------
// The SAME logic - "if (!mutex) mutex = xSemaphoreCreateMutex()" followed by
// take/give - was duplicated across FOUR different modules (notification.cpp,
// tab5_camera.cpp, app_trilha/trilha_background.cpp, and the camera bridge in
// kernel/sistema.cpp), with each copy reimplementing the same pair of functions.
// Only ONE of them (notification.cpp) took the time to explain why it was safe;
// the other three repeated the code without repeating the reasoning.
// Consolidating it here does not change any behavior - it simply ensures that
// the risk described below is understood once, rather than forgotten in three places.
//
// ------------------------------------------------------------------------
// THE RISK (fixed - see tab5_mutex_criar below), documented
// ------------------------------------------------------------------------
// If TWO tasks call tab5_mutex_travar() FOR THE FIRST TIME simultaneously,
// both may see a null pointer, each creates ITS OWN mutex, and the last write
// to the shared pointer wins - both tasks believe they have acquired the lock,
// but they are holding DIFFERENT objects, so mutual exclusion simply did not
// happen (worse than having no mutex at all: it appears safe).
//
// tab5_sd.cpp already encountered this risk in practice and resolved it
// DIFFERENTLY: it creates the mutex once, in tab5_sd_init(), called before
// competition exists - see the comment there. DO NOT change tab5_sd.cpp to
// use this approach: its design choice is deliberate and better suited to its
// use case (it has a clear, single boot phase where the mutex can be created
// safely).
//
// Here, the OPPOSITE approach is intentional, for the reason originally
// documented in notification.cpp: do not impose an initialization order on
// modules that use this functionality.
// This remains safe only as long as the FIRST call at each call site continues
// to happen BEFORE actual competition exists:
// - notification.cpp: services register one at a time during boot, from
// the same task;
// - tab5_camera.cpp and the bridge in kernel/core.cpp: every first call
// occurs in (or is handled by) the UI task - never by two genuinely
// concurrent tasks;
// - trail_background.cpp: the first brake_mapa() call occurs inside
// ao_initiar(), BEFORE the kernel creates the "Trilha" task.
//
// IF THIS EVER STOPS BEING TRUE - for example, a SECOND camera consumer
// outside the UI task, or a service registering from its own task before
// boot finishes - change that call site to the approach used by tab5_sd.cpp
// (mutex created in an explicit _initiar() function, called early and without
// competition) instead of continuing to rely on this approach.
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// RACE-FREE CREATION. The mutex is created OUTSIDE the critical section
// (allocation cannot be performed inside it) and published INSIDE it:
// if two tasks reach this point simultaneously for the first time, both
// create a mutex, but only the first publishes its mutex, while the other
// deletes the one it created and uses the published mutex. The race condition
// described above no longer exists - the rule "first call before competition
// begins" remains good practice, but mutual exclusion no longer depends on it.
inline void tab5_mutex_create(SemaphoreHandle_t* m)
{
  if (*m) { return; }
  SemaphoreHandle_t novo = xSemaphoreCreateMutex();
  if (!novo) { return; }
  static portMUX_TYPE s_publicar = portMUX_INITIALIZER_UNLOCKED;
  taskENTER_CRITICAL(&s_publicar);
  if (!*m) { *m = novo; novo = nullptr; }
  taskEXIT_CRITICAL(&s_publicar);
  if (novo) { vSemaphoreDelete(novo); }
}

inline void tab5_mutex_lock(SemaphoreHandle_t* m)
{
  tab5_mutex_create(m);
  if (*m)  { xSemaphoreTake(*m, portMAX_DELAY); }
}

inline void tab5_mutex_destravar(SemaphoreHandle_t* m)
{
  if (*m) { xSemaphoreGive(*m); }
}
