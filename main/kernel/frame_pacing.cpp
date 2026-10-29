#include "frame_pacing.hpp"

#include "esp_timer.h"
#include "tab5_frame.hpp"

// Start a little AFTER scanning: the ISR that notes it has already run, and the
// change of page requested in this frame can only be valid in the following one.
static const int64_t FRAME_HEADROOM_US = 1500;
// Drawing that starts less than this of next scan is not ready to
// time: goes on air in another (and the dt already exits correctly).
static const int64_t FRAME_DEADLINE_MARGIN_US = 3000;

static const void* s_dono     = nullptr;
static int64_t     s_on_ar    = 0;     // when the frame in progress goes live (estimated)
static int64_t     s_prox_us  = 0;     // when the next periodic begins (0 = none)
static float       s_dt       = 0.033f;

static uint32_t periodo_panel(void)
{
  uint32_t p = 0;
  tab5_frame_delays(nullptr, &p);
  return (p >= 8000 && p <= 60000) ? p : 0;
}

void pacing_frame(const void* dono, uint32_t periodo_ms)
{
  const int64_t t  = esp_timer_get_time();
  const int64_t P  = periodo_panel();
  const int64_t vs = tab5_frame_last_end_us();
  const int64_t request = (int64_t)periodo_ms * 1000;

  // No measurement of panel (boot, just after wake): the clock of before.
  const bool measured = P && vs > 0 && t >= vs && t - vs < 1000000;
  int64_t on_ar = t;
  if (measured) {
    on_ar = vs + ((t - vs) / P + 1) * P;   // the next scan
    if (on_ar - t < FRAME_DEADLINE_MARGIN_US) { on_ar += P; }
  }

  const int64_t ant = (dono == s_dono) ? s_on_ar : 0;
  // Two frames will never air in the same scan: the second waits for the exchange
  // of first (comp_abrir).
  if (measured && ant && on_ar < ant + P) { on_ar = ant + P; }
  s_dt = ant ? (float)(on_ar - ant) * 1e-6f
             : (request ? (float)request * 1e-6f : 0.033f);
  if (s_dt < 0.001f) { s_dt = 0.001f; }
  s_dono  = dono;
  s_on_ar = on_ar;

  if (!periodo_ms) { s_prox_us = 0; return; }
  if (measured) {
    int64_t n = (request + P / 2) / P;      // entire scans, closest to request
    if (n < 1) { n = 1; }
    s_prox_us = on_ar + (n - 1) * P + FRAME_HEADROOM_US;
  } else {
    s_prox_us = t + request;
  }
}

uint32_t pacing_falta_ms(void)
{
  if (!s_prox_us) { return 0; }
  const int64_t t = esp_timer_get_time();
  if (t >= s_prox_us) { return 0; }
  return (uint32_t)((s_prox_us - t + 999) / 1000);
}

float pacing_dt(void) { return s_dt; }
