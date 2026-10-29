// APP ENERGIA - the status of battery now and two graphs in the form of
// cell phones (a screen of battery of Android and of iOS):
//
// 1. THE CHOSEN GRIDNESS over time (by default the level of battery),
// with filled area, axis with round numbers and hours of CHARGE
// dyed in green behind the curve: loading and unloading are read by the shape,
// without having to read number. Period 30 MIN (ring of RAM) or 24 H.
// 2. SPEND PER HOUR (mAh) in the last 24 hours, in bars: screen lit at top,
// sleep below, and a green stripe below the charging hours.
//
// In the 24 hours the two graphs share the SAME axis of time (one column per
// time of clock, the last and the current time): the bar remains below the
// part of the curve it refers to.
//
// At top: the large percentage with the estimate ("hard ~X" / "full in ~X")
// and one INDEX of six cards with the values of moment. Touch a card
// chooses the magnitude of graph 1. Tapping graph 1 puts the cursor with the
// time and value; Touching a bar shows the detail of that time.
//
// 24-hour data: the files of yesterday and today (services/energy_log, a
// sample per minute, with the real time), read by the WORKER, completed with the
// ring of 30 min of RAM - what the registry has not yet written (lots of 5 min).
//
// Redesign by REGION: top (hero + cards) every second; both
// graphics, with titles, only when new data arrives or a selection changes.
// No touch requires pass complete: each region clears its own rectangle.

#include "app.h"
#include "tab5_screen.hpp"
#include "paint.hpp"
#include "tab5_sd.hpp"
#include "tab5_ui.hpp"
#include "tab5_time.hpp"
#include "work.hpp"
#include "energy_log.h"
#include "tab5_power.hpp"
#include "tab5_sensors.hpp"
#include "esp_heap_caps.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

typedef enum { SERIE_BATERIA = 0, SERIE_CORRENTE, SERIE_POTENCIA, SERIE_TENSAO, SERIE_TEMP } serie_t;
#define N_SERIES 5
static const char* SERIE_TITULO[N_SERIES] = { "Battery level (%)", "Current (mA)", "Power (W)", "Voltage (V)", "P4 temperature (°C)" };
static const char* SERIE_CARTAO[N_SERIES] = { "Battery", "Current", "Power", "Voltage", "P4 temp" };
static const uint16_t SERIE_COR[N_SERIES] = {
  lgfx::color565( 90, 170, 255),   // battery: blue, so as not to be confused with the green of charge
  TFT_ORANGE,
  TFT_GREENYELLOW,
  lgfx::color565(190, 150, 255),
  lgfx::color565(255,  90,  80),
};

#define COR_CARGA        lgfx::color565( 60, 200,  90)   // load: same green throughout the app
#define COR_CARGA_FUNDO  lgfx::color565( 14,  50,  26)   // ink of hours of charge behind of curve
#define COR_GASTO_TELA   TFT_ORANGE
#define COR_GASTO_REP    lgfx::color565(130,  75,  20)
#define COR_CARTAO       lgfx::color565( 26,  26,  34)
#define COR_CARTAO_SEL   lgfx::color565( 42,  42,  56)
#define COR_EIXO         lgfx::color565(110, 110, 124)

static serie_t s_serie   = SERIE_BATERIA;
static bool    s_24h     = true;      // graph 1: true = 24 H (card + RAM); false = 30 MIN (RAM)
static int     s_cursor  = -1;        // x of cursor on graph 1 (relative to area); -1 = no
static int     s_hora_sel = -1;       // bar chosen on chart 2 (0..23); -1 = totals
static bool    s_graf_dirty = true;    // one touch changed what the graphics show
static paint_t s_p;

// -------------------------------------------------------------------------------
// 24 H: one slot per minute of window, loaded by worker
// -------------------------------------------------------------------------------
// The window starts at full hour of 23 hours ago: 24 columns of full hour, the
// last being the current (partial) time. Align in hora of clock and what
// leaves the bars of graph 2 with labels "14h", "15h" instead of slices of
// 60 minutes that start at any minute.
#define MIN_DIA 1440
typedef struct {
  uint32_t t0;                 // UTC epoch of slot 0 (0 = no clock)
  float    v[MIN_DIA], ma[MIN_DIA], temp[MIN_DIA];
  int8_t   pct[MIN_DIA];       // the % that the system showed (estimator), -1 = without
  uint8_t  tem[MIN_DIA];       // 0 = no sample, 1 = awake, 2 = sleep
} dia_t;

// A source of points: n points, x of 0..1, the values, and the instant 't'
// (24 H: the slot, minute since t0; 30 MIN: seconds before now).
typedef struct {
  int     n;
  float   x[MIN_DIA], v[MIN_DIA], ma[MIN_DIA], temp[MIN_DIA];
  int8_t  pct[MIN_DIA];
  uint8_t rep[MIN_DIA];        // 2 = in sleep; other = screen on (or not known)
  bool    quebra[MIN_DIA];     // hole before this point: device disabled
  int32_t t[MIN_DIA];
} pontos_t;

// One vertex per column of pixel of graph 1 (see assemble_vertices).
#define VERT_MAX 720
typedef struct {
  int     n;
  int16_t px[VERT_MAX];
  float   val[VERT_MAX];
  bool    carga[VERT_MAX], quebra[VERT_MAX];
  int32_t t[VERT_MAX];
} vertices_t;

typedef struct { float screen, sleep, carga; int min_screen; bool tem; } hora_t;

// Everything that the interface uses, in a block in PSRAM allocated on open.
typedef struct {
  pontos_t     p24, p30;
  vertices_t   vt;
  tab5_point_t ram[TAB5_HIST_N];
  hora_t       horas[24];
  float        tot_gasto, tot_carga, tot_wh;
  int          tot_min_screen;
} memoria_t;
static memoria_t* s_m = nullptr;

static dia_t*        s_dia = nullptr;      // what the screen shows
static dia_t*        s_carga = nullptr;    // where the worker writes
static char          s_carga_name[2][40];  // yesterday and today (could be the same)
static uint32_t      s_carga_t0 = 0;
static volatile bool s_carga_pronta = false;
static bool          s_carga_requested = false;
static uint32_t      s_gravadas_lidas = UINT32_MAX;   // record already reflected in 24 hours
static uint32_t      s_dia_ver = 0;                    // goes up with each new load

// Start of window of 24 h for the current instant; 0 without clock set.
static uint32_t window_t0(void)
{
  const uint32_t now = tab5_time_epoch();
  if (!now) { return 0; }
  struct tm lt;
  tab5_local_time(now, &lt);
  return now - (uint32_t)(lt.tm_min * 60 + lt.tm_sec) - 23u * 3600u;
}

static void read_day_file(dia_t* d, const char* name)
{
  FILE* f = tab5_sd_open(TAB5_SD_POWER, name, "r");
  if (!f) { return; }
  char line[160];
  while (fgets(line, sizeof(line), f)) {
    unsigned long ep; char hora[8]; float v, ma, mw, temp; int pct, mhz, rep;
    if (sscanf(line, "ENE3,%lu,%7[^,],%f,%f,%f,%d,%d,%d,%f", &ep, hora, &v, &ma, &mw, &pct, &mhz, &rep, &temp) < 9) { continue; }
    if (ep < d->t0) { continue; }
    const uint32_t m = ((uint32_t)ep - d->t0) / 60;
    if (m >= MIN_DIA) { continue; }
    d->tem[m] = rep ? 2 : 1;
    d->v[m] = v; d->ma[m] = ma; d->temp[m] = temp;
    d->pct[m] = (int8_t)((pct >= 0 && pct <= 100) ? pct : -1);
  }
  fclose(f);
}

static void work_24h(void*)
{
  dia_t* d = s_carga;
  memset(d, 0, sizeof(*d));
  d->t0 = s_carga_t0;
  if (d->t0 && tab5_sd_mount()) {
    read_day_file(d, s_carga_name[0]);
    if (strcmp(s_carga_name[0], s_carga_name[1]) != 0) { read_day_file(d, s_carga_name[1]); }
    tab5_sd_unmount();
  }
  s_carga_pronta = true;
}

static void request_24h(void)
{
  if (s_carga_requested || !s_carga) { return; }
  s_carga_t0 = window_t0();
  s_carga_name[0][0] = s_carga_name[1][0] = 0;
  if (s_carga_t0) {
    tab5_day_filename(s_carga_t0, "energia-", ".csv", s_carga_name[0], sizeof(s_carga_name[0]));
    tab5_day_filename((uint32_t)time(nullptr), "energia-", ".csv", s_carga_name[1], sizeof(s_carga_name[1]));
  }
  s_carga_pronta = false;
  s_carga_requested = system_work(work_24h, nullptr);
}

static void on_notice(uint32_t av)
{
  if ((av & AVISO_TRABALHO) && s_carga_pronta) {
    s_carga_pronta = false;
    s_carga_requested = false;
    dia_t* t = s_dia; s_dia = s_carga; s_carga = t;
    ++s_dia_ver;
    system_request_redraw();
  }
}

static bool on_open(void)
{
  if (!s_dia)   { s_dia   = (dia_t*)heap_caps_calloc(1, sizeof(dia_t), MALLOC_CAP_SPIRAM); }
  if (!s_carga) { s_carga = (dia_t*)heap_caps_calloc(1, sizeof(dia_t), MALLOC_CAP_SPIRAM); }
  if (!s_m)     { s_m     = (memoria_t*)heap_caps_calloc(1, sizeof(memoria_t), MALLOC_CAP_SPIRAM); }
  if (!s_dia || !s_carga || !s_m) { return false; }
  s_carga_requested = false;
  s_carga_pronta = false;
  s_gravadas_lidas = UINT32_MAX;   // 24 hours are read on open
  s_cursor = -1;
  s_hora_sel = -1;
  s_graf_dirty = true;
  paint_request(&s_p);
  return true;
}

static void on_close(void)
{
  // (The system already waited for the worker before calling this.)
  heap_caps_free(s_dia);   s_dia = nullptr;
  heap_caps_free(s_carga); s_carga = nullptr;
  heap_caps_free(s_m);     s_m = nullptr;
}

// -------------------------------------------------------------------------------
// GEOMETRY (relative to area, 720 x 1144 in portrait; ends at ~1120) - the same for drawing and touch
// -------------------------------------------------------------------------------
static const int M = 16;                               // side margin
static const int HERO_Y = 8, HERO_A = 76;              // % large + status
static const int CART_Y = HERO_Y + HERO_A + 8;         // index: 3 x 2 cards
static const int CART_A = 92, CART_V = 12;             // height and go between cards
static const int TOPO_A = CART_Y + 2 * CART_A + CART_V;
static const int EIXO_L = 64;                          // column of numbers of Y axis

static const int G1_Y = TOPO_A + 20;                   // title + 30 MIN | 24H
static const int SEG_A = 48, SEG_L = 240;
static const int G1_PLOT_Y = G1_Y + SEG_A + 16, G1_A = 320;
static const int G1_FIM = G1_PLOT_Y + G1_A + 34;       // + labels of time

static const int G2_Y = G1_FIM + 22;                   // title + caption
static const int G2_PLOT_Y = G2_Y + 48, G2_A = 216;
static const int G2_FAIXA = 8;                         // green stripe of cargo under the bars
static const int G2_DET_Y = G2_PLOT_Y + G2_A + G2_FAIXA + 40;   // two lines of detail

static int cart_l(int larg) { return (larg - 2 * M - 2 * CART_V) / 3; }
static int seg_x(int larg)  { return larg - M - SEG_L; }
static int plot_x(void)     { return M + EIXO_L; }
static int plot_l(int larg) { return larg - 2 * M - EIXO_L; }

static const float TAM_BOTAO = 3.625f;     // = body 29 px native

// System soft fonts, at native heights (not scaled).
static void fc(lgfx::LovyanGFX& d, int px) { d.setFont(tab5_fonte_suave(false, px)); d.setTextSize(1); }
static void fn(lgfx::LovyanGFX& d, int px) { d.setFont(tab5_fonte_suave(true, px));  d.setTextSize(1); }

// Darkened color (f of 0..256) - the filling of area under the curve.
static uint16_t escurecer(uint16_t c, int f)
{
  const int r = ((c >> 11) & 31) * f / 256, g = ((c >> 5) & 63) * f / 256, b = (c & 31) * f / 256;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t color_level(int pct)
{
  return pct < 0 ? (uint16_t)TFT_DARKGREY : pct < 20 ? (uint16_t)TFT_RED
       : pct < 50 ? (uint16_t)TFT_YELLOW : (uint16_t)COR_CARGA;
}

static void duracao(char* out, size_t size, float horas)
{
  const int min = (int)(horas * 60.0f + 0.5f);
  if (min < 60) { snprintf(out, size, "%d min", min); }
  else          { snprintf(out, size, "%d h %02d min", min / 60, min % 60); }
}

// -------------------------------------------------------------------------------
// SERIES
// -------------------------------------------------------------------------------
// The BATTERY series is the SAME % that the system shows (the estimator, recorded at
// each sample) - not the voltage curve recalculated here, which would say otherwise
// thing right after a load and would put the graph at odds with the bar.
static float valor(const pontos_t* p, serie_t s, int i)
{
  switch (s) {
    case SERIE_TENSAO:   return p->v[i];
    case SERIE_CORRENTE: return p->ma[i];                                       // WITH signal
    case SERIE_POTENCIA: return p->v[i] * fabsf(p->ma[i]) / 1000.0f;            //W
    case SERIE_TEMP:     return p->temp[i];
    default:             return (float)p->pct[i];
  }
}
static bool valido(const pontos_t* p, serie_t s, int i)
{
  if (s == SERIE_TEMP)    { return p->temp[i] > -200.0f; }
  if (s == SERIE_BATERIA) { return p->pct[i] >= 0; }
  return p->v[i] >= 1.0f;
}
static const char* formato(serie_t s)
{
  return (s == SERIE_TENSAO)   ? "%.2f V"
       : (s == SERIE_CORRENTE) ? "%+.0f mA"
       : (s == SERIE_POTENCIA) ? "%.2f W"
       : (s == SERIE_TEMP)     ? "%.1f °C" : "%.0f %%";
}
static float minimum_axis_range(serie_t s)
{
  return (s == SERIE_TENSAO) ? 0.10f : (s == SERIE_CORRENTE) ? 40.0f
       : (s == SERIE_POTENCIA) ? 0.2f : 4.0f;
}

// SCALE WITH ROUND NUMBERS. The old chart only showed the minimum and maximum
// measured (e.g.: -183 and -97 mA) in the corner: none line of grade had value,
// and the curve could not be read. Here the limits are multiples of one step 1/2/2.5/5,
// and each line of grid has its number on the axis.
static void escala(float mn, float mx, float minima, float* lo, float* hi, float* passo)
{
  if (mx - mn < minima) { const float c = (mx + mn) / 2; mn = c - minima / 2; mx = c + minima / 2; }
  const float bruto = (mx - mn) / 3.0f;
  const float mag = powf(10.0f, floorf(log10f(bruto)));
  const float r = bruto / mag;
  *passo = mag * (r <= 1.0f ? 1.0f : r <= 2.0f ? 2.0f : r <= 2.5f ? 2.5f : r <= 5.0f ? 5.0f : 10.0f);
  *lo = floorf(mn / *passo + 1e-4f) * *passo;
  *hi = ceilf(mx / *passo - 1e-4f) * *passo;
  if (*hi <= *lo) { *hi = *lo + *passo; }
}

static void format_axis_tick(char* out, size_t size, float v, float passo)
{
  if (fabsf(v) < passo * 1e-3f) { v = 0; }   // nothing of "-0"
  snprintf(out, size, passo >= 1.0f ? "%.0f" : passo >= 0.1f ? "%.1f" : "%.2f", (double)v);
}

// -------------------------------------------------------------------------------
// MOUNTING THE POINTS
// -------------------------------------------------------------------------------
static void build_30_second_points(pontos_t* p)
{
  const int n = tab5_power_history(s_m->ram);
  for (int i = 0; i < n; ++i) {
    // Aligned to right: "now" always in edge, filling in the first 30 min.
    p->x[i] = (float)(TAB5_HIST_N - n + i) / (TAB5_HIST_N - 1);
    p->v[i] = s_m->ram[i].volts; p->ma[i] = s_m->ram[i].mA; p->temp[i] = s_m->ram[i].temp; p->pct[i] = (int8_t)s_m->ram[i].pct;
    p->rep[i] = 0;
    p->quebra[i] = (i == 0);
    p->t[i] = (n - 1 - i) * TAB5_HIST_SEG;
  }
  p->n = n;
}

static void build_24_hour_points(pontos_t* p)
{
  p->n = 0;
  if (!s_dia || !s_dia->t0) { return; }
  int ultimo = -1000;
  for (int m = 0; m < MIN_DIA; ++m) {
    if (!s_dia->tem[m]) { continue; }
    const int k = p->n++;
    p->x[k] = (float)m / MIN_DIA;
    p->v[k] = s_dia->v[m]; p->ma[k] = s_dia->ma[m]; p->temp[k] = s_dia->temp[m]; p->pct[k] = s_dia->pct[m];
    p->rep[k] = s_dia->tem[m];
    p->quebra[k] = (m - ultimo) > 3;   // hole of more than 3 min: device disabled
    p->t[k] = m;
    ultimo = m;
  }
  // WHAT THE CARD DOESN'T YET HAVE: the record records in batches of 5 minutes, so
  // complete with the ring of RAM, from the next minute to the last recorded until
  // now. The ring does not keep sleep; these few minutes count as
  // screen on (the app is open, after all).
  const uint32_t now = tab5_time_epoch();
  if (!now) { return; }
  const int n = tab5_power_history(s_m->ram);
  for (int i = 0; i < n; ++i) {
    const uint32_t ep = (uint32_t)now - (uint32_t)((n - 1 - i) * TAB5_HIST_SEG);
    if (ep < s_dia->t0) { continue; }
    const int m = (int)((ep - s_dia->t0) / 60);
    if (m <= ultimo || m >= MIN_DIA || p->n >= MIN_DIA) { continue; }
    const int k = p->n++;
    p->x[k] = (float)m / MIN_DIA;
    p->v[k] = s_m->ram[i].volts; p->ma[k] = s_m->ram[i].mA; p->temp[k] = s_m->ram[i].temp; p->pct[k] = (int8_t)s_m->ram[i].pct;
    p->rep[k] = 0;
    p->quebra[k] = (m - ultimo) > 3;
    p->t[k] = m;
    ultimo = m;
  }
}

// Each point of 24 hours is worth one minute: mAh = mA / 60.
static void build_hour_markers(const pontos_t* p)
{
  memset(s_m->horas, 0, sizeof(s_m->horas));
  s_m->tot_gasto = s_m->tot_carga = s_m->tot_wh = 0;
  s_m->tot_min_screen = 0;
  for (int i = 0; i < p->n; ++i) {
    hora_t* h = &s_m->horas[p->t[i] / 60];
    h->tem = true;
    const bool screen = (p->rep[i] != 2);
    if (screen) { ++h->min_screen; ++s_m->tot_min_screen; }
    if (p->ma[i] < 0) {
      const float mah = -p->ma[i] / 60.0f;
      if (screen) { h->screen += mah; } else { h->sleep += mah; }
      s_m->tot_gasto += mah;
      s_m->tot_wh += p->v[i] * mah / 1000.0f;
    } else {
      h->carga += p->ma[i] / 60.0f;
      s_m->tot_carga += p->ma[i] / 60.0f;
    }
  }
}

// ONE VERTEX PER PIXEL COLUMN. In 24 hours there are 1440 minutes at ~600 px:
// several points fall in the same column, and their average becomes the vertex. This
// smooths out the noise minute by minute of current (which turned into a blur of
// zigzag) without a separate filter, and the cost of drawing depends on
// width, not of number of points.
static void build_plot_vertices(const pontos_t* p, serie_t s, int gx, int gl)
{
  vertices_t* v = &s_m->vt;
  v->n = 0;
  int cnt = 0; float soma = 0; bool quebra_pend = true;
  for (int i = 0; i < p->n; ++i) {
    if (!valido(p, s, i)) { quebra_pend = true; continue; }
    const int px = gx + (int)(p->x[i] * (gl - 1) + 0.5f);
    if (p->quebra[i]) { quebra_pend = true; }
    if (cnt && px == v->px[v->n - 1] && !quebra_pend) {
      soma += valor(p, s, i); ++cnt;
      v->val[v->n - 1] = soma / cnt;
      v->carga[v->n - 1] |= (p->ma[i] > 20.0f);
      v->t[v->n - 1] = p->t[i];
      continue;
    }
    if (v->n >= VERT_MAX) { break; }
    const int k = v->n++;
    v->px[k] = (int16_t)px; v->val[k] = valor(p, s, i);
    v->carga[k] = (p->ma[i] > 20.0f);
    v->quebra[k] = quebra_pend;
    v->t[k] = p->t[i];
    soma = v->val[k]; cnt = 1; quebra_pend = false;
  }
}

// -------------------------------------------------------------------------------
// DRAWING - TOP (hero + index), every second
// -------------------------------------------------------------------------------
// Battery drawn: contour, filling by %, radius by top in charge.
static void icon_battery(lgfx::LovyanGFX& d, int x, int y, int l, int a, int pct, bool loading)
{
  const uint16_t color = color_level(pct);
  d.fillRoundRect(x, y, l, a, 7, TFT_LIGHTGREY);
  d.fillRoundRect(x + 3, y + 3, l - 6, a - 6, 5, TFT_BLACK);
  d.fillRoundRect(x + l + 2, y + a / 2 - 7, 5, 14, 2, TFT_LIGHTGREY);
  const int cheio = pct < 0 ? 0 : (l - 12) * (pct > 100 ? 100 : pct) / 100;
  if (cheio > 0) { d.fillRoundRect(x + 6, y + 6, cheio, a - 12, 3, color); }
  if (loading) {
    const int cx = x + l / 2, cy = y + a / 2;
    d.fillTriangle(cx + 4, cy - 14, cx - 9, cy + 2, cx + 1, cy + 2, TFT_WHITE);
    d.fillTriangle(cx - 4, cy + 14, cx + 9, cy - 2, cx - 1, cy - 2, TFT_WHITE);
  }
}

static void draw_topo(lgfx::LovyanGFX& d, const area_t& a)
{
  const tab5_measurement_t m = tab5_power_latest();
  tab5_screen_clear(a.x, a.y, a.width, TOPO_A, TFT_BLACK);
  char buf[64], dur[24];

  // HERO. The state comes from the current of instant (the same rule of three
  // status bar); the ESTIMATE comes from the average of the last
  // 30 min - the current of one second jumps too much to become a prediction.
  const int n = tab5_power_history(s_m->ram);
  float media = m.mA;
  if (n >= 4) { media = 0; for (int i = 0; i < n; ++i) { media += s_m->ram[i].mA; } media /= n; }
  const bool loading = m.mA > 20.0f, in_battery = m.mA < -20.0f;

  const int y = a.y + HERO_Y;
  fn(d, 56); d.setTextDatum(top_left);
  d.setTextColor(color_level(m.pct), TFT_BLACK);
  if (m.pct < 0) { snprintf(buf, sizeof(buf), "--"); } else { snprintf(buf, sizeof(buf), "%d%%", m.pct); }
  d.drawString(buf, a.x + M, y);
  const int end_pct = a.x + M + d.textWidth(buf);
  icon_battery(d, end_pct + 18, y + (d.fontHeight() - 34) / 2, 68, 34, m.pct, loading);
  // Tapping the hero chooses the DRUMS series: the track on the left says it is chosen.
  if (s_serie == SERIE_BATERIA) { d.fillRoundRect(a.x + 4, y + 8, 5, HERO_A - 24, 2, SERIE_COR[SERIE_BATERIA]); }

  const char* estado; uint16_t color_estado; buf[0] = 0;
  if (loading) {
    estado = "Carregando"; color_estado = COR_CARGA;
    if (media > 20.0f && m.pct >= 0 && m.pct < 100) {
      duracao(dur, sizeof(dur), (100 - m.pct) / 100.0f * TAB5_CAPACITY_MAH / media);
      snprintf(buf, sizeof(buf), "cheia em ~%s", dur);
    }
  } else if (in_battery) {
    estado = "Na bateria"; color_estado = TFT_WHITE;
    if (media < -20.0f && m.pct >= 0) {
      const float h = m.pct / 100.0f * TAB5_CAPACITY_MAH / -media;
      if (h < 100.0f) { duracao(dur, sizeof(dur), h); snprintf(buf, sizeof(buf), "dura ~%s neste ritmo", dur); }
    }
  } else {
    estado = "Na tomada"; color_estado = COR_CARGA;
    snprintf(buf, sizeof(buf), "carga completa");
  }
  const int xr = a.x + a.width - M;
  fn(d, 32); d.setTextDatum(top_right); d.setTextColor(color_estado, TFT_BLACK);
  d.drawString(estado, xr, y + 2);
  fc(d, 24); d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString(buf, xr, y + 42);

  // INDEX: six cards. The first five are the series - touch chooses the
  // graph 1; the sixth is the expenditure of 24 hours (the total in graph 2).
  float temp = -273.0f; const bool tem_temp = tab5_sensors_temp(&temp);
  const int cl = cart_l(a.width);
  for (int i = 0; i < 6; ++i) {
    const int cx = a.x + M + (i % 3) * (cl + CART_V), cy = a.y + CART_Y + (i / 3) * (CART_A + CART_V);
    const bool sel = (i < N_SERIES && (int)s_serie == i);
    const uint16_t fundo = sel ? COR_CARTAO_SEL : COR_CARTAO;
    d.fillRoundRect(cx, cy, cl, CART_A, 12, fundo);
    if (sel) {
      d.drawRoundRect(cx, cy, cl, CART_A, 12, SERIE_COR[i]);
      d.drawRoundRect(cx + 1, cy + 1, cl - 2, CART_A - 2, 11, SERIE_COR[i]);
    }
    uint16_t color = (i < N_SERIES) ? SERIE_COR[i] : (uint16_t)TFT_WHITE;
    switch (i) {
      case SERIE_BATERIA:  if (m.pct < 0) { snprintf(buf, sizeof(buf), "--"); } else { snprintf(buf, sizeof(buf), "%d %%", m.pct); } break;
      case SERIE_CORRENTE: snprintf(buf, sizeof(buf), "%+.0f mA", (double)m.mA); color = m.mA > 20.0f ? (uint16_t)COR_CARGA : color; break;
      case SERIE_POTENCIA: snprintf(buf, sizeof(buf), "%.2f W", (double)(m.mW / 1000.0f)); break;
      case SERIE_TENSAO:   snprintf(buf, sizeof(buf), "%.2f V", (double)m.volts); break;
      case SERIE_TEMP:     if (tem_temp) { snprintf(buf, sizeof(buf), "%.1f °C", (double)temp); } else { snprintf(buf, sizeof(buf), "--"); } break;
      default:             if (s_dia && s_dia->t0) { snprintf(buf, sizeof(buf), "%.0f mAh", (double)s_m->tot_gasto); } else { snprintf(buf, sizeof(buf), "--"); } break;
    }
    fc(d, 21); d.setTextDatum(top_left);
    d.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, fundo);
    d.drawString(i < N_SERIES ? SERIE_CARTAO[i] : "Gasto 24 h", cx + 14, cy + 10);
    fn(d, 36); d.setTextColor(color, fundo);
    d.drawString(buf, cx + 14, cy + 40);
  }
  d.setTextDatum(top_left);
}

// -------------------------------------------------------------------------------
// DRAWING - TIME AXIS (the same for both graphs in the 24 h)
// -------------------------------------------------------------------------------
static void eixo_time(lgfx::LovyanGFX& d, int gx, int gl, int gy, int ga, bool h24, bool grade)
{
  const int divs = h24 ? 4 : 2;     // 24H: every 6h; 30 MIN: every 15 min
  if (grade) { for (int k = 1; k < divs; ++k) { d.drawFastVLine(gx + (gl * k) / divs, gy, ga, TAB5_GRAFICO_COR_GRADE); } }
  fc(d, 21); d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  static const char* T[3] = { "-30 min", "-15 min", "agora" };
  char buf[12];
  for (int k = 0; k <= divs; ++k) {
    const char* txt = T[k < 3 ? k : 2];
    if (h24) {
      if (k == divs) { txt = "agora"; }
      else {
        struct tm lt; tab5_local_time(s_dia->t0 + (uint32_t)k * 6u * 3600u, &lt);
        snprintf(buf, sizeof(buf), "%dh", lt.tm_hour); txt = buf;
      }
    }
    d.setTextDatum(k == 0 ? top_left : k == divs ? top_right : top_center);
    d.drawString(txt, gx + (gl * k) / divs, gy + ga + 8);
  }
  d.setTextDatum(top_left);
}

static void draw_graph_notice(lgfx::LovyanGFX& d, int gx, int gl, int gy, int ga, const char* txt)
{
  fc(d, 24); d.setTextColor(TFT_DARKGREY, TFT_BLACK); d.setTextDatum(middle_center);
  d.drawString(txt, gx + gl / 2, gy + ga / 2);
  d.setTextDatum(top_left);
}

static void draw_eixo_y(lgfx::LovyanGFX& d, int gx, int gl, int gy, int ga, float lo, float hi, float passo, bool so_middle_cheio)
{
  char buf[16];
  fc(d, 21); d.setTextColor(TFT_LIGHTGREY, TFT_BLACK); d.setTextDatum(middle_right);
  const int n = (int)((hi - lo) / passo + 0.5f);
  for (int k = 0; k <= n; ++k) {
    const float v = lo + k * passo;
    const int y = gy + ga - 1 - (int)((v - lo) * (ga - 1) / (hi - lo) + 0.5f);
    d.drawFastHLine(gx, y, gl, k == 0 ? COR_EIXO : TAB5_GRAFICO_COR_GRADE);
    if (so_middle_cheio && (k % 2)) { continue; }   // battery: labels 0/50/100, grid every 25
    format_axis_tick(buf, sizeof(buf), v, passo);
    d.drawString(buf, gx - 10, y);
  }
  d.setTextDatum(top_left);
}

// -------------------------------------------------------------------------------
// DRAWING - GRAPH 1: the magnitude chosen over time
// -------------------------------------------------------------------------------
static void draw_grafico1(lgfx::LovyanGFX& d, const area_t& a)
{
  const int gx = a.x + plot_x(), gl = plot_l(a.width), gy = a.y + G1_PLOT_Y, ga = G1_A;
  tab5_screen_clear(a.x, a.y + G1_Y, a.width, G1_FIM - G1_Y, TFT_BLACK);
  char buf[48];

  // Header: what is the graph (with unit) and the period.
  fn(d, 28); d.setTextDatum(middle_left); d.setTextColor(SERIE_COR[s_serie], TFT_BLACK);
  d.drawString(SERIE_TITULO[s_serie], a.x + M, a.y + G1_Y + SEG_A / 2);
  const int sx = a.x + seg_x(a.width), sy = a.y + G1_Y, middle = SEG_L / 2;
  tab5_botao(d, sx,        sy, middle, SEG_A, "30 MIN", s_24h ? (uint16_t)TFT_BLACK : (uint16_t)TFT_WHITE,
             s_24h ? (uint16_t)TFT_LIGHTGREY : (uint16_t)TFT_BLACK, TAM_BOTAO);
  tab5_botao(d, sx + middle, sy, middle, SEG_A, "24 H",   s_24h ? (uint16_t)TFT_WHITE : (uint16_t)TFT_BLACK,
             s_24h ? (uint16_t)TFT_BLACK : (uint16_t)TFT_LIGHTGREY, TAM_BOTAO);
  d.drawRoundRect(sx, sy, SEG_L, SEG_A, 10, TFT_LIGHTGREY);
  d.setTextDatum(top_left);

  if (s_24h && (!s_dia || !s_dia->t0)) {
    d.drawFastHLine(gx, gy + ga - 1, gl, COR_EIXO);
    draw_graph_notice(d, gx, gl, gy, ga, s_carga_requested ? "reading the card..." : "24 H needs the clock (GPS, mesh, or NTP)");
    return;
  }
  const pontos_t* p = s_24h ? &s_m->p24 : &s_m->p30;
  eixo_time(d, gx, gl, gy, ga, s_24h, false);
  build_plot_vertices(p, s_serie, gx, gl);
  vertices_t* v = &s_m->vt;
  if (v->n < 2) {
    d.drawFastHLine(gx, gy + ga - 1, gl, COR_EIXO);
    draw_graph_notice(d, gx, gl, gy, ga, s_24h ? "ainda sem pontos (1 por minuto)" : "juntando dados (1 ponto a cada 30 s)...");
    return;
  }

  float lo, hi, passo;
  if (s_serie == SERIE_BATERIA) { lo = 0; hi = 100; passo = 25; }
  else {
    float mn = v->val[0], mx = v->val[0];
    for (int i = 1; i < v->n; ++i) { if (v->val[i] < mn) { mn = v->val[i]; } if (v->val[i] > mx) { mx = v->val[i]; } }
    // Current: zero comes into scale - the line of zero separates charge from discharge.
    if (s_serie == SERIE_CORRENTE) { if (mn > 0) { mn = 0; } if (mx < 0) { mx = 0; } }
    escala(mn, mx, minimum_axis_range(s_serie), &lo, &hi, &passo);
  }
  #define YV(val) (gy + ga - 1 - (int)((((val) < lo ? lo : (val) > hi ? hi : (val)) - lo) * (ga - 1) / (hi - lo) + 0.5f))

  // 1) The hours of LOAD dyed behind everything (in current the signal itself
  // already says this, with the green fill above zero).
  if (s_serie != SERIE_CORRENTE) {
    for (int i = 0; i < v->n; ++i) {
      if (!v->carga[i]) { continue; }
      const bool liga = (i + 1 < v->n && !v->quebra[i + 1]);
      const int x1 = liga ? v->px[i + 1] : v->px[i] + 2;
      d.fillRect(v->px[i], gy, x1 - v->px[i] + 1, ga, COR_CARGA_FUNDO);
    }
  }
  // 2) Grid with numbers and the vertical of time.
  draw_eixo_y(d, gx, gl, gy, ga, lo, hi, passo, s_serie == SERIE_BATERIA);
  eixo_time(d, gx, gl, gy, ga, s_24h, true);

  // 3) AREA under the curve, column by column, interpolating between vertices.
  // Current: fills to ZERO - green above (load), orange below.
  const uint16_t color = SERIE_COR[s_serie];
  const int y_base = (s_serie == SERIE_CORRENTE) ? YV(0.0f) : gy + ga - 2;
  const uint16_t fill = escurecer(color, 72);
  const uint16_t fill_carga = escurecer(COR_CARGA, 80);
  for (int i = 1; i < v->n; ++i) {
    if (v->quebra[i]) { continue; }
    const int x0 = v->px[i - 1], x1 = v->px[i];
    const float y0 = (float)YV(v->val[i - 1]), y1 = (float)YV(v->val[i]);
    for (int x = x0; x <= x1; ++x) {
      const int y = (x1 == x0) ? (int)y1 : (int)(y0 + (y1 - y0) * (x - x0) / (x1 - x0) + 0.5f);
      if (y < y_base) { d.drawFastVLine(x, y, y_base - y, s_serie == SERIE_CORRENTE ? fill_carga : fill); }
      else if (y > y_base) { d.drawFastVLine(x, y_base + 1, y - y_base, fill); }
    }
  }
  if (s_serie == SERIE_CORRENTE) { d.drawFastHLine(gx, y_base, gl, TFT_LIGHTGREY); }

  // 4) THE LINE per top (~3 px, edge softened). Isolated point becomes a ball.
  for (int i = 0; i < v->n; ++i) {
    const int y = YV(v->val[i]);
    if (i > 0 && !v->quebra[i]) { tab5_line_grossa(d, v->px[i - 1], YV(v->val[i - 1]), v->px[i], y, 1.5f, color); }
    else if (i + 1 >= v->n || v->quebra[i + 1]) { d.fillCircle(v->px[i], y, 2, color); }
  }

  // 5) CURSOR: the vertex closest to touch, with the time and value.
  if (s_cursor >= 0) {
    const int alvo = a.x + s_cursor;
    int k = 0;
    for (int i = 1; i < v->n; ++i) { if (abs(v->px[i] - alvo) < abs(v->px[k] - alvo)) { k = i; } }
    const int cx = v->px[k], cy = YV(v->val[k]);
    d.drawFastVLine(cx, gy, ga, TFT_WHITE);
    d.fillCircle(cx, cy, 7, TFT_WHITE);
    d.fillCircle(cx, cy, 4, color);
    char val[24];
    snprintf(val, sizeof(val), formato(s_serie), (double)v->val[k]);
    if (s_24h) {
      struct tm lt; tab5_local_time(s_dia->t0 + (uint32_t)v->t[k] * 60u, &lt);
      snprintf(buf, sizeof(buf), "%02d:%02d   %s", lt.tm_hour, lt.tm_min, val);
    } else {
      const int s = v->t[k];
      if (s < 60) { snprintf(buf, sizeof(buf), "agora   %s", val); }
      else        { snprintf(buf, sizeof(buf), "%d min ago   %s", s / 60, val); }
    }
    fn(d, 24);
    const int bl = d.textWidth(buf) + 24, ba = 40;
    int bx = cx + 12;
    if (bx + bl > gx + gl) { bx = cx - 12 - bl; }
    if (bx < gx) { bx = gx; }
    d.fillRoundRect(bx, gy + 6, bl, ba, 8, COR_CARTAO_SEL);
    d.drawRoundRect(bx, gy + 6, bl, ba, 8, color);
    d.setTextDatum(middle_left); d.setTextColor(TFT_WHITE, COR_CARTAO_SEL);
    d.drawString(buf, bx + 12, gy + 6 + ba / 2);
    d.setTextDatum(top_left);
  }
  #undef YV
}

// -------------------------------------------------------------------------------
// DRAWING - GRAPH 2: spent per hour in 24 hours
// -------------------------------------------------------------------------------
static void quadradinho(lgfx::LovyanGFX& d, int* xr, int ym, uint16_t color, const char* txt)
{
  // Caption written from right to left: returns in *xr where next ends.
  const int w = d.textWidth(txt);
  d.setTextDatum(middle_right); d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString(txt, *xr, ym);
  d.fillRoundRect(*xr - w - 22, ym - 8, 16, 16, 3, color);
  *xr -= w + 22 + 20;
}

static void draw_grafico2(lgfx::LovyanGFX& d, const area_t& a)
{
  const int gx = a.x + plot_x(), gl = plot_l(a.width), gy = a.y + G2_PLOT_Y, ga = G2_A;
  tab5_screen_clear(a.x, a.y + G2_Y, a.width, a.height - G2_Y, TFT_BLACK);
  char buf[96], dur[24];

  fn(d, 28); d.setTextDatum(middle_left); d.setTextColor(COR_GASTO_TELA, TFT_BLACK);
  d.drawString("Gasto por hora (mAh)", a.x + M, a.y + G2_Y + 18);
  fc(d, 18);
  int xr = a.x + a.width - M;
  quadradinho(d, &xr, a.y + G2_Y + 18, COR_CARGA, "carga");
  quadradinho(d, &xr, a.y + G2_Y + 18, COR_GASTO_REP, "repouso");
  quadradinho(d, &xr, a.y + G2_Y + 18, COR_GASTO_TELA, "tela");
  d.setTextDatum(top_left);

  if (!s_dia || !s_dia->t0) {
    d.drawFastHLine(gx, gy + ga - 1, gl, COR_EIXO);
    draw_graph_notice(d, gx, gl, gy, ga, s_carga_requested ? "reading the card..." : "needs the clock (GPS, mesh, or NTP)");
    return;
  }

  float mx = 0;
  for (int h = 0; h < 24; ++h) { const float g = s_m->horas[h].screen + s_m->horas[h].sleep; if (g > mx) { mx = g; } }
  float lo, hi, passo;
  escala(0, mx < 20.0f ? 20.0f : mx, 20.0f, &lo, &hi, &passo);
  lo = 0;
  draw_eixo_y(d, gx, gl, gy, ga, lo, hi, passo, false);

  // Bars aligned with columns of hour of chart 1 (same gx/gl).
  for (int h = 0; h < 24; ++h) {
    const int c0 = gx + (gl * h) / 24, c1 = gx + (gl * (h + 1)) / 24;
    const int bx = c0 + 3, bl = c1 - c0 - 6;
    const hora_t* hr = &s_m->horas[h];
    if (h == s_hora_sel) { d.fillRect(c0 + 1, gy, c1 - c0 - 2, ga - 1, COR_CARTAO_SEL); }
    const int a_rep  = (int)(hr->sleep * (ga - 2) / hi + 0.5f);
    const int a_screen = (int)(hr->screen    * (ga - 2) / hi + 0.5f);
    const int base = gy + ga - 2;
    if (a_rep > 0)  { d.fillRect(bx, base - a_rep + 1, bl, a_rep, COR_GASTO_REP); }
    if (a_screen > 0) { d.fillRect(bx, base - a_rep - a_screen + 1, bl, a_screen, COR_GASTO_TELA); }
    if (hr->carga > 5.0f) { d.fillRoundRect(bx, gy + ga + 2, bl, G2_FAIXA - 2, 2, COR_CARGA); }
  }
  eixo_time(d, gx, gl, gy + G2_FAIXA, ga, true, false);

  // DETAIL: the time played, or the 24-hour totals.
  const int y1 = a.y + G2_DET_Y, y2 = y1 + 32;
  d.setTextDatum(top_left);
  if (s_hora_sel >= 0) {
    const hora_t* hr = &s_m->horas[s_hora_sel];
    struct tm lt; tab5_local_time(s_dia->t0 + (uint32_t)s_hora_sel * 3600u, &lt);
    snprintf(buf, sizeof(buf), "%dh - %dh: gastou %.0f mAh · carregou %.0f mAh",
             lt.tm_hour, (lt.tm_hour + 1) % 24, (double)(hr->screen + hr->sleep), (double)hr->carga);
    fn(d, 24); d.setTextColor(TFT_WHITE, TFT_BLACK); d.drawString(buf, a.x + M, y1);
    if (hr->tem) {
      snprintf(buf, sizeof(buf), "tela acesa %d min (%.0f mAh)   repouso %.0f mAh",
               hr->min_screen, (double)hr->screen, (double)hr->sleep);
    } else {
      snprintf(buf, sizeof(buf), "sem registro nesta hora (aparelho desligado)");
    }
    fc(d, 24); d.setTextColor(TFT_LIGHTGREY, TFT_BLACK); d.drawString(buf, a.x + M, y2);
  } else {
    snprintf(buf, sizeof(buf), "24 h: gastou %.0f mAh (%.2f Wh) · carregou %.0f mAh",
             (double)s_m->tot_gasto, (double)s_m->tot_wh, (double)s_m->tot_carga);
    fn(d, 24); d.setTextColor(TFT_WHITE, TFT_BLACK); d.drawString(buf, a.x + M, y1);
    duracao(dur, sizeof(dur), s_m->tot_min_screen / 60.0f);
    uint32_t falhas; registro_estado(nullptr, nullptr, &falhas);
    if (falhas) { snprintf(buf, sizeof(buf), "screen on %s   log: %lu card failures", dur, (unsigned long)falhas); }
    else        { snprintf(buf, sizeof(buf), "tela acesa %s   toque numa barra para ver a hora", dur); }
    fc(d, 24); d.setTextColor(falhas ? TFT_ORANGE : TFT_LIGHTGREY, TFT_BLACK); d.drawString(buf, a.x + M, y2);
  }
}

static void draw(lgfx::LovyanGFX& d, const area_t& a)
{
  const auto estilo = d.getTextStyle();
  const lgfx::IFont* fonte = d.getFont();
  paint_start(&s_p);
  const bool everything = paint_complete(&s_p);

  // 24 H: reloads when the record records a new sample (every 5 min) or
  // when the time turned (the window moves one column).
  uint32_t grav; registro_estado(nullptr, &grav, nullptr);
  if (grav != s_gravadas_lidas || (s_dia && window_t0() != s_dia->t0)) { s_gravadas_lidas = grav; request_24h(); }

  if (everything) { tab5_screen_clear(a.x, a.y, a.width, a.height, TFT_BLACK); }

  // The graphs only change when data arrives (ring every TAB5_HIST_SEG, card
  // with each load) or when one touch changed the selection.
  static int s_assin = -1;
  const int n = tab5_power_history(s_m->ram);
  const int assin = (int)s_dia_ver * 7919 + n * 1000003 + (n ? (int)(s_m->ram[n - 1].mA * 10) : 0);
  if (everything || s_graf_dirty || assin != s_assin) {
    s_assin = assin;
    s_graf_dirty = false;
    build_24_hour_points(&s_m->p24);
    build_hour_markers(&s_m->p24);
    build_30_second_points(&s_m->p30);
    draw_grafico1(d, a);
    draw_grafico2(d, a);
  }
  draw_topo(d, a);   // later: the "Spent 24 h" card reads the total that set_hours just made

  d.setFont(fonte);
  d.setTextStyle(estilo);
}

static void on_touch(int x, int y)
{
  const area_t a = system_area();
  const int cl = cart_l(a.width);
  const int gx = plot_x(), gl = plot_l(a.width);
  if (y >= HERO_Y && y < HERO_Y + HERO_A) {
    s_serie = SERIE_BATERIA; s_cursor = -1;
  }
  // An index card: chooses chart series 1 (the sixth only tells you).
  else if (y >= CART_Y && y < TOPO_A && x >= M) {
    const int col = (x - M) / (cl + CART_V), lin = (y - CART_Y) / (CART_A + CART_V);
    const int i = lin * 3 + (col > 2 ? 2 : col);
    if (i >= N_SERIES) { return; }
    s_serie = (serie_t)i; s_cursor = -1;
  }
  // Period control: half left 30 MIN, right 24 H.
  else if (y >= G1_Y && y < G1_Y + SEG_A && x >= seg_x(a.width)) {
    s_24h = (x >= seg_x(a.width) + SEG_L / 2); s_cursor = -1;
    if (s_24h) { request_24h(); }
  }
  // Inside graph 1: place the cursor; touch near him of new strip.
  else if (y >= G1_PLOT_Y && y < G1_PLOT_Y + G1_A && x >= gx - 8 && x < gx + gl + 8) {
    s_cursor = (s_cursor >= 0 && abs(x - s_cursor) < 16) ? -1 : x;
  }
  // A bar of graph 2 (with the load range and labels below).
  else if (y >= G2_PLOT_Y && y < G2_PLOT_Y + G2_A + G2_FAIXA + 34 && x >= gx && x < gx + gl) {
    const int h = (x - gx) * 24 / gl;
    s_hora_sel = (h == s_hora_sel) ? -1 : h;
  }
  else { return; }
  s_graf_dirty = true;
  system_request_redraw();
}


// -------------------------------------------------------------------------------
// HOME SCREEN ICON
// -------------------------------------------------------------------------------
// Primitives and fraction of side, like the of Mesh tag: clean exit on any
// size, no flash and no serration. The big letter that was here was
// the embedded font magnified 6x - each pixel of it became a square of 6.
static void draw_energy_icon(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t fundo  = d.color565(255, 150, 40);
  const uint16_t escuro = d.color565(55, 30, 0);

  d.fillRoundRect(x, y, lado, lado, lado / 6, fundo);

  #define IX(f) (x + (int)((f) * lado))
  #define IY(f) (y + (int)((f) * lado))

  // Stack lying down with the pole to the right. Thick contour drawn like two
  // rectangles (full and hollow), which is cheaper than four lines and does not
  // leaves corner open.
  d.fillRoundRect(IX(0.14f), IY(0.32f), (int)(0.62f * lado), (int)(0.36f * lado),
                  lado / 16, escuro);
  d.fillRoundRect(IX(0.14f) + lado / 22, IY(0.32f) + lado / 22,
                  (int)(0.62f * lado) - lado / 11, (int)(0.36f * lado) - lado / 11,
                  lado / 20, fundo);
  d.fillRoundRect(IX(0.78f), IY(0.42f), (int)(0.07f * lado), (int)(0.16f * lado),
                  lado / 40, escuro);

  // Lightning inside: and what separates "battery" from "consumption". The app measures current,
  // not just position.
  d.fillTriangle(IX(0.48f), IY(0.36f), IX(0.30f), IY(0.53f), IX(0.44f), IY(0.53f), escuro);
  d.fillTriangle(IX(0.40f), IY(0.64f), IX(0.58f), IY(0.47f), IX(0.44f), IY(0.47f), escuro);

  #undef IX
  #undef IY
}

extern const app_t APP_ENERGY = {
  .name  = "Energy",
  .color   = TFT_ORANGE,
  // Profile: just the brightness. Everything else comes from the defaults of tab5_profile_t,
  // which are 'disabled' - so there is no need to list what is not used.
  .profile = { },
  .sleep_clock_floor_mhz = 0,  // without work of background: can lower on minimum
  .on_open  = on_open,
  .on_close = on_close,
  .draw  = draw,
  .on_touch  = on_touch,
  .redraw_period_ms = 1000,     // the top follows the measurement of 1Hz of kernel
  .notices = AVISO_TRABALHO,
  .on_notice = on_notice,
  .icon = draw_energy_icon,
};
