#include "compositor.hpp"

#include "tab5_screen.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <atomic>

static const char *TAG = "comp";

static const int FB_L = 720;
static const int FB_A = 1280;

// -------------------------------------------------------------------------------
// DIRTY AREAS
// -------------------------------------------------------------------------------
// Merging two areas is only valid when the rectangle that surrounds them is not larger than
// the two added together - otherwise we redesign a pixel that no one asked for. And the same
// rule of LVGL (lv_refr.c, refr_join_area).
//
// 32 and not 16: a screen with scattered text (top bar, values in the middle,
// bar of bottom) fills 16 easily, and what happens when filling costs
// expensive - see burst().
//
// 96 and no 32: ANIMATION WITH MANY SMALL OBJECTS. Particles, flakes,
// stars - each one is a little box, and 32 filled in the middle of an explosion of
// fireworks. Full, the list joins distant boxes into fat rectangles, and the
// next page copies all the remainder: the frame weighed just when a screen
// filled up, and the animation choked. Those who animate it had to put together their own
// boxes to fit here - be careful as it is not of app. Costs 1.5 KB of RAM
// extra internal (three lists); the path of each writing remains short
// (see s_ult_contem).
#define MAX_AREAS 96

typedef struct { tab5_rect_t r[MAX_AREAS]; int n; } areas_t;

static areas_t s_sujas;      // what will be drawn in the next frame
static areas_t s_anterior;   // what the previous frame drew (missing in page that exited)
static uint32_t s_estouros = 0;

static inline int area(const tab5_rect_t& r) { return r.l * r.a; }

static tab5_rect_t envolver(const tab5_rect_t& a, const tab5_rect_t& b)
{
  const int x0 = a.x < b.x ? a.x : b.x;
  const int y0 = a.y < b.y ? a.y : b.y;
  const int x1 = (a.x + a.l) > (b.x + b.l) ? (a.x + a.l) : (b.x + b.l);
  const int y1 = (a.y + a.a) > (b.y + b.a) ? (a.y + a.a) : (b.y + b.a);
  return { (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
}

// How much of pixel does NOT request the fusion of a and b requires copying.
static inline int desperdicio(const tab5_rect_t& a, const tab5_rect_t& b)
{
  return area(envolver(a, b)) - area(a) - area(b);
}

static bool contains(const tab5_rect_t& fora, const tab5_rect_t& dentro)
{
  return dentro.x >= fora.x && dentro.y >= fora.y
      && dentro.x + dentro.l <= fora.x + fora.l
      && dentro.y + dentro.a <= fora.y + fora.a;
}

static bool recortar_in_screen(tab5_rect_t& r)
{
  int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
  int x1 = r.x + r.l > FB_L ? FB_L : r.x + r.l;
  int y1 = r.y + r.a > FB_A ? FB_A : r.y + r.a;
  if (x1 <= x0 || y1 <= y0) { return false; }
  r = { (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
  return true;
}

// The area covered by the last write. Almost all writing falls into the same
// area than the previous one (the lines of one circle, the glyphs of one text, the
// drawing by top of box that has just been deleted): look at it first
// saves scanning the entire list, which is now long.
static int s_ult_contains = 0;

void comp_invalidar(const tab5_rect_t& entrada)
{
  tab5_rect_t r = entrada;
  if (!recortar_in_screen(r)) { return; }
  if (s_ult_contains < s_sujas.n && contains(s_sujas.r[s_ult_contains], r)) { return; }

  for (;;) {
    // Already covered: nothing to do.
    for (int i = 0; i < s_sujas.n; ++i) { if (contains(s_sujas.r[i], r)) { s_ult_contains = i; return; } }

    // Merge while compensating. One merger can enable another, then repeat.
    bool fundiu = true;
    while (fundiu) {
      fundiu = false;
      for (int i = 0; i < s_sujas.n; ++i) {
        if (desperdicio(s_sujas.r[i], r) <= 0) {
          r = envolver(s_sujas.r[i], r);
          s_sujas.r[i] = s_sujas.r[--s_sujas.n];
          fundiu = true;
          break;
        }
      }
    }

    if (s_sujas.n < MAX_AREAS) { s_sujas.r[s_sujas.n++] = r; return; }

    // LIST FULL. The first version involved EVERYTHING in just one rectangle - and the
    // bar of top plus one number at the bottom turned the entire screen (1.8 MB
    // of copies for a handful of text). Now the new area joins the NEIGHBOR
    // that wastes less: it opens up space without adding weight to what is far away. (A
    // previous version looked for the best PAR among all - n^2 for each burst,
    // too expensive with the long list; with 96 vacancies the overflow became rare and the
    // difference of left between the two ways, small.)
    ++s_estouros;
    int bi = 0;
    int melhor = 0x7fffffff;
    for (int i = 0; i < s_sujas.n; ++i) {
      const int dr = desperdicio(s_sujas.r[i], r);
      if (dr < melhor) { melhor = dr; bi = i; }
    }
    r = envolver(s_sujas.r[bi], r);
    s_sujas.r[bi] = s_sujas.r[--s_sujas.n];
    // Back to top: the merged area can now cover or merge with others.
  }
}

// -------------------------------------------------------------------------------
// FRAME STATUS
// -------------------------------------------------------------------------------
static bool     s_open   = false;   // the behind page is synchronized and in use
static bool     s_active    = false;   // we know the pages of panel current
static bool     s_pending = false;   // we asked for an exchange and we haven't confirmed it yet
static uint32_t s_geracao  = 0;       // of which panel are the pages we know
static uint32_t s_geracao_vista = 0;  // last panel announced on core (comp_panel_novo)
static void*    s_tras     = nullptr; // the page behind frame open
static uint32_t s_epoca    = 0;       // g_tab5_screen_epoch when the frame opened
static uint32_t s_round   = 0;
static int64_t  s_ult_frame_us = 0;  // diagnostics: when the last frame exited
static uint32_t s_ult_frame_px = 0;  // and how many pixels it changed
static std::atomic<bool> s_request_everything{false};

// LAZY SYNCHRONY. The areas of frame before the page behind
// DOESN'T have it yet. They are not copied on open: each one comes from the front only when
// something will touch it (writing or reading - comp_preparar), and whatever is left,
// in time to present. The one that is completely overwritten by a copy of DMA
// (scroll, surface) is never copied.
//
// Why: the first version synchronized EVERYTHING on open. But the frame opens
// in first writing of iteration - and apps draw the header before of
// list. When the scroll arrived saying "I'm going to cover the entire window", the
// window had already been copied: measured, the sync was 2/3 of the entire copy.
static areas_t s_pend;
static void*   s_frente_pg = nullptr;   // the front page of frame open

static comp_estat_t s_est = {};       // a measurement (see comp_estat)
static uint64_t     s_us_dentro = 0;  // time spent in here (open + copies)
static uint64_t     s_marca_dentro = 0;

static bool intersecta(const tab5_rect_t& a, const tab5_rect_t& b)
{
  return a.x < b.x + b.l && b.x < a.x + a.l && a.y < b.y + b.a && b.y < a.y + a.a;
}

// Every copy of composer passes through here: this is where the time of DMA is counted.
static bool copiar(void* destino, const tab5_rect_t& r, const void* origem, int ox, int oy)
{
  const int64_t t0 = esp_timer_get_time();
  const bool ok = tab5_frame_copy_from(destino, r, origem, ox, oy);
  const uint64_t dt = (uint64_t)(esp_timer_get_time() - t0);
  s_est.us_copia += dt;
  s_us_dentro += dt;
  return ok;
}

static void wait_troca_pending(void)
{
  if (!s_pending) { return; }
  const int64_t t0 = esp_timer_get_time();
  if (!tab5_frame_wait_for_swap(50)) { ESP_LOGW(TAG, "troca nao confirmada em 50 ms"); }
  const uint64_t dt = (uint64_t)(esp_timer_get_time() - t0);
  s_est.us_espera += dt;
  s_us_dentro += dt;
  s_pending = false;
}

// Discards the frame in progress: the panel disappeared (sleep) or was changed.
// What was drawn is lost of purpose - the core redesigns everything
// when the new panel appears (comp_panel_novo).
static void descartar(void)
{
  s_open = false;
  s_tras = nullptr;
  s_frente_pg = nullptr;
  s_sujas.n = 0;
  s_pend.n = 0;
  // New round: the shortcut of tracker ("this rectangle has already been noted and
  // prepared this round") cannot cross over to the next frame - it
  // would skip the comp_preparar, and the synchronization of presentation would erase the drawing.
  ++s_round;
}

// THE PENDANTS ARE CUT, not copied whole. The merger of areas together
// neighbors (the header of one app and the window of list below become ONE
// rectangle pending). Copy the entire pending because the header was
// touched brought the window together - 1.6 MB that scrolling then overwrites.
// Now only the INTERSECTION comes (or is discarded), and the rest of pending continues
// pending, in up to 4 pieces.
static tab5_rect_t intersecao(const tab5_rect_t& a, const tab5_rect_t& b)
{
  const int x0 = a.x > b.x ? a.x : b.x;
  const int y0 = a.y > b.y ? a.y : b.y;
  const int x1 = (a.x + a.l) < (b.x + b.l) ? (a.x + a.l) : (b.x + b.l);
  const int y1 = (a.y + a.a) < (b.y + b.a) ? (a.y + a.a) : (b.y + b.a);
  return { (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
}

// Removes pending i and returns to the list what stays OUTSIDE of 'r' (range of
// top, of bottom, left, right). false = did not fit in list: nothing was
// changed, and whoever called copies the entire pending.
static bool recortar_pending(int i, const tab5_rect_t& r)
{
  const tab5_rect_t p = s_pend.r[i];
  const tab5_rect_t c = intersecao(p, r);
  tab5_rect_t pedaco[4];
  int n = 0;
  if (c.y > p.y)                 { pedaco[n++] = { p.x, p.y, p.l, (int16_t)(c.y - p.y) }; }
  if (c.y + c.a < p.y + p.a)     { pedaco[n++] = { p.x, (int16_t)(c.y + c.a), p.l, (int16_t)(p.y + p.a - c.y - c.a) }; }
  if (c.x > p.x)                 { pedaco[n++] = { p.x, c.y, (int16_t)(c.x - p.x), c.a }; }
  if (c.x + c.l < p.x + p.l)     { pedaco[n++] = { (int16_t)(c.x + c.l), c.y, (int16_t)(p.x + p.l - c.x - c.l), c.a }; }
  if (s_pend.n - 1 + n > MAX_AREAS) { return false; }
  s_pend.r[i] = s_pend.r[--s_pend.n];
  for (int k = 0; k < n; ++k) { s_pend.r[s_pend.n++] = pedaco[k]; }
  return true;
}

static void copiar_pending(const tab5_rect_t& a)
{
  copiar(s_tras, a, s_frente_pg, a.x, a.y);
  s_est.px_sinc += (uint32_t)area(a);
}

// Brings the entire pending i and the strip of list (swaps with the last one - who
// runs backwards without losing anyone).
static void synchronize(int i)
{
  const tab5_rect_t a = s_pend.r[i];
  s_pend.r[i] = s_pend.r[--s_pend.n];
  copiar_pending(a);
}

// Scrolls backwards: the pieces added at the end are OUT
// of 'r' and do not need to be revised.
void comp_preparar(const tab5_rect_t& r)
{
  if (!s_open || s_pend.n == 0) { return; }
  for (int i = s_pend.n - 1; i >= 0; --i) {
    if (i >= s_pend.n || !intersecta(s_pend.r[i], r)) { continue; }
    const tab5_rect_t c = intersecao(s_pend.r[i], r);
    if (recortar_pending(i, r)) { copiar_pending(c); }
    else                         { synchronize(i); }
  }
}

void comp_cobrir(const tab5_rect_t& r)
{
  if (!s_open) { return; }
  for (int i = s_pend.n - 1; i >= 0; --i) {
    if (i >= s_pend.n || !intersecta(s_pend.r[i], r)) { continue; }
    if (contains(r, s_pend.r[i])) { s_pend.r[i] = s_pend.r[--s_pend.n]; continue; }
    if (!recortar_pending(i, r)) { synchronize(i); }   // no space: comes in one piece (correct, they are more expensive)
  }
}

void* comp_abrir(void)
{
  if (s_open) {
    // THE PATH OF ALL WRITING (each pixel of edge of letter passes here):
    // a comparison, nothing more. An open frame cannot cross the
    // change of panel (sleep, clock) - his page was released together
    // with the old panel -, and the time of the pages goes up exactly to that.
    if (s_epoca == g_tab5_screen_epoch) { return s_tras; }
    descartar();
  }

  // No panel (sleep): nothing to open.
  if (!tab5_screen_init()) { return nullptr; }

  // NEW PANEL (sleep, change of clock, boot): the pages that
  // we knew no longer exist, and the new ones have nothing of ours - there is no
  // that synchronizes. The core has already been (or will be) warned by
  // comp_panel_novo() and redraws the entire screen.
  if (!s_active || tab5_screen_generation() != s_geracao) {
    s_geracao = tab5_screen_generation();
    s_active = true;
    s_pending = false;
    s_anterior.n = 0;
  }

  // 1. The back page is only free after the panel has changed.
  wait_troca_pending();
  void* tras   = tab5_screen_page(true);
  void* frente = tab5_screen_page(false);
  if (!tras || !frente) { return nullptr; }

  // 2. Synchrony, LAZY: what the previous frame drew stays
  // pending, and comes forward only when touched (see s_pend).
  s_pend = s_anterior;
  s_anterior.n = 0;

  s_tras = tras;
  s_frente_pg = frente;
  s_epoca = g_tab5_screen_epoch;
  s_open = true;
  return s_tras;
}

bool comp_desenhado_neste_quadro(const tab5_rect_t& src)
{
  for (int i = 0; i < s_sujas.n; ++i) { if (intersecta(s_sujas.r[i], src)) { return true; } }
  return false;
}

bool comp_copiar_da_frente(const tab5_rect_t& r, int ox, int oy)
{
  void* tras = comp_abrir();
  if (!tras) { return false; }
  if (!s_frente_pg || s_frente_pg == tras) { return false; }
  if (!copiar(tras, r, s_frente_pg, ox, oy)) { return false; }
  comp_cobrir(r);   // only AFTER it works: it failed, the pending issues continue
  s_est.px_rolar_dma += (uint32_t)area(r);
  return true;
}

bool comp_copiar_imagem(const void* imagem, const tab5_rect_t& r)
{
  void* tras = comp_abrir();
  if (!tras || !imagem) { return false; }
  if (!copiar(tras, r, imagem, r.x, r.y)) { return false; }
  comp_cobrir(r);
  comp_invalidar(r);
  return true;
}

void comp_invalidar_tudo(void) { s_request_everything.store(true, std::memory_order_relaxed); }

bool comp_apresentar(void)
{
  if (s_request_everything.exchange(false, std::memory_order_relaxed)) {
    comp_invalidar({ 0, 0, (int16_t)FB_L, (int16_t)FB_A });
  }
  if (s_sujas.n == 0) { return false; }

  // Marked without anything drawn (so one request of invalidate): open here, to
  // the back page will be the same as the front page before going live.
  if (!s_open && !comp_abrir()) { descartar(); return false; }

  // The panel disappeared or was changed AFTER opening: the page where
  // we drew no longer exists.
  if (!tab5_screen_init() || tab5_screen_generation() != s_geracao) { descartar(); return false; }

  // What no one touched is still pending: come now, otherwise the back page
  // would go live with the frame of two changes ago at these points.
  while (s_pend.n > 0) { synchronize(s_pend.n - 1); }

  int y_min = FB_A, y_max = 0;
  uint32_t px = 0;
  for (int k = 0; k < s_sujas.n; ++k) {
    const tab5_rect_t& a = s_sujas.r[k];
    if (a.y < y_min) { y_min = a.y; }
    if (a.y + a.a > y_max) { y_max = a.y + a.a; }
    px += (uint32_t)area(a);
  }

  // 3. The exchange. The driver returns in cache the lines y_min..y_max of page
  // of back (what the CPU wrote and is still only in the cache) and changes the
  // index. If you refuse, the frame stays open and tries again in next
  // iteration - close here would make the synchronization erase what was drawn.
  if (!tab5_screen_request_swap(y_min, y_max)) { return false; }
  s_pending = true;
  s_anterior = s_sujas;
  s_sujas.n = 0;
  s_open = false;
  s_tras = nullptr;
  s_frente_pg = nullptr;
  ++s_round;

  s_ult_frame_us = esp_timer_get_time();
  s_ult_frame_px = px;
  ++s_est.frames;
  s_est.px_frame += px;
  if (px >= (uint32_t)FB_L * FB_A / 10 * 9) { ++s_est.cheios; }
  return true;
}

bool comp_painel_novo(void)
{
  if (!tab5_screen_init()) { return false; }
  const uint32_t g = tab5_screen_generation();
  if (g == s_geracao_vista) { return false; }
  s_geracao_vista = g;
  return true;
}

bool comp_tem_sujeira(void) { return s_sujas.n > 0 || s_request_everything.load(std::memory_order_relaxed); }
void comp_esperar_troca(void) { wait_troca_pending(); }
uint32_t comp_rodada(void) { return s_round; }

void comp_ultimo_quadro(int64_t* when_us, uint32_t* px) { if (when_us) { *when_us = s_ult_frame_us; } if (px) { *px = s_ult_frame_px; } }

void comp_marcar_desenho(void) { s_marca_dentro = s_us_dentro; }

uint32_t comp_contar_desenho(uint32_t us, bool complete)
{
  const uint64_t dentro = s_us_dentro - s_marca_dentro;
  const uint64_t cpu = (us > dentro) ? (us - dentro) : 0;
  s_est.us_drawing += cpu;
  ++s_est.n_drawing;
  if (complete) { s_est.us_drawing_complete += cpu; ++s_est.n_drawing_complete; }
  return (uint32_t)cpu;
}

void comp_contar_volta(uint32_t us)
{
  // The MEASURED period of panel (hal/tab5_frame), not a fixed number: the
  // clock of panel and adjustment of user (59 MHz = 20.6 ms, 80 MHz = 15.7).
  uint32_t periodo = 0;
  tab5_frame_delays(nullptr, &periodo);
  if (periodo == 0) { periodo = 16667; }
  uint32_t f = us / periodo;          // 0 = fit in 1 period
  if (f > COMP_FAIXAS - 1) { f = COMP_FAIXAS - 1; }
  s_est.us_iteration += us;
  ++s_est.voltas[f];
}

// Reading of another task (console, monitor) without crash: 64-bit fields
// can exit ripped in a rare instant. And diagnostics - a crooked number in a
// window disappears next; a lock here would weigh down the entire frame.
void comp_estat(comp_estat_t* out)
{
  if (!out) { return; }
  *out = s_est;
  out->estouros = s_estouros;
  tab5_screen_scroll_cost(&out->px_rolados, &out->us_rolar);
}
