#include "frame_pacing.hpp"

#include "esp_timer.h"
#include "tab5_frame.hpp"

// Comecar one pouco DEPOIS of varredura: a ISR que a anota ja rodou, e a
// troca of pagina requested neste frame so pode valer in seguinte.
static const int64_t FOLGA_US = 1500;
// Desenho que comeca a menos disto of next varredura nao stays pronto a
// time: vai on ar in outra (e o dt ja exit certo).
static const int64_t EM_CIMA_US = 3000;

static const void* s_dono     = nullptr;
static int64_t     s_on_ar    = 0;     // when o frame em andamento vai on ar (estimado)
static int64_t     s_prox_us  = 0;     // when comecar o next periodico (0 = nenhum)
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

  // Sem measurement of panel (boot, logo depois of wake): o clock of antes.
  const bool measured = P && vs > 0 && t >= vs && t - vs < 1000000;
  int64_t on_ar = t;
  if (measured) {
    on_ar = vs + ((t - vs) / P + 1) * P;   // a next varredura
    if (on_ar - t < EM_CIMA_US) { on_ar += P; }
  }

  const int64_t ant = (dono == s_dono) ? s_on_ar : 0;
  // Dois quadros nunca vao on ar in mesma varredura: o segundo espera a troca
  // of first (comp_abrir).
  if (measured && ant && on_ar < ant + P) { on_ar = ant + P; }
  s_dt = ant ? (float)(on_ar - ant) * 1e-6f
             : (request ? (float)request * 1e-6f : 0.033f);
  if (s_dt < 0.001f) { s_dt = 0.001f; }
  s_dono  = dono;
  s_on_ar = on_ar;

  if (!periodo_ms) { s_prox_us = 0; return; }
  if (measured) {
    int64_t n = (request + P / 2) / P;      // varreduras inteiras, o mais perto of request
    if (n < 1) { n = 1; }
    s_prox_us = on_ar + (n - 1) * P + FOLGA_US;
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
