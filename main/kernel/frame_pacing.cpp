#include "frame_pacing.hpp"

#include "esp_timer.h"
#include "tab5_frame.hpp"

// Comecar um pouco DEPOIS da varredura: a ISR que a anota ja rodou, e a
// troca de pagina pedida neste quadro so pode valer na seguinte.
static const int64_t FOLGA_US = 1500;
// Desenho que comeca a menos disto da proxima varredura nao fica pronto a
// tempo: vai ao ar na outra (e o dt ja sai certo).
static const int64_t EM_CIMA_US = 3000;

static const void* s_dono     = nullptr;
static int64_t     s_ao_ar    = 0;     // quando o quadro em andamento vai ao ar (estimado)
static int64_t     s_prox_us  = 0;     // quando comecar o proximo periodico (0 = nenhum)
static float       s_dt       = 0.033f;

static uint32_t periodo_painel(void)
{
  uint32_t p = 0;
  tab5_frame_delays(nullptr, &p);
  return (p >= 8000 && p <= 60000) ? p : 0;
}

void ritmo_quadro(const void* dono, uint32_t periodo_ms)
{
  const int64_t t  = esp_timer_get_time();
  const int64_t P  = periodo_painel();
  const int64_t vs = tab5_frame_last_end_us();
  const int64_t pedido = (int64_t)periodo_ms * 1000;

  // Sem medida do painel (boot, logo depois de acordar): o relogio de antes.
  const bool medido = P && vs > 0 && t >= vs && t - vs < 1000000;
  int64_t ao_ar = t;
  if (medido) {
    ao_ar = vs + ((t - vs) / P + 1) * P;   // a proxima varredura
    if (ao_ar - t < EM_CIMA_US) { ao_ar += P; }
  }

  const int64_t ant = (dono == s_dono) ? s_ao_ar : 0;
  // Dois quadros nunca vao ao ar na mesma varredura: o segundo espera a troca
  // do primeiro (comp_abrir).
  if (medido && ant && ao_ar < ant + P) { ao_ar = ant + P; }
  s_dt = ant ? (float)(ao_ar - ant) * 1e-6f
             : (pedido ? (float)pedido * 1e-6f : 0.033f);
  if (s_dt < 0.001f) { s_dt = 0.001f; }
  s_dono  = dono;
  s_ao_ar = ao_ar;

  if (!periodo_ms) { s_prox_us = 0; return; }
  if (medido) {
    int64_t n = (pedido + P / 2) / P;      // varreduras inteiras, o mais perto do pedido
    if (n < 1) { n = 1; }
    s_prox_us = ao_ar + (n - 1) * P + FOLGA_US;
  } else {
    s_prox_us = t + pedido;
  }
}

uint32_t ritmo_falta_ms(void)
{
  if (!s_prox_us) { return 0; }
  const int64_t t = esp_timer_get_time();
  if (t >= s_prox_us) { return 0; }
  return (uint32_t)((s_prox_us - t + 999) / 1000);
}

float ritmo_dt(void) { return s_dt; }
