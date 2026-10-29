#include "list.hpp"
#include "internal.hpp"   // diag_toque_on
#include "esp_log.h"
#include "app.h"   // system_area - the touch arrives at coordinates of area

static const int BARRA_MARGEM = 8;   // scroll_bar draws 4px to 8px of border
static const int BARRA_LARG   = 4;

void list_set(list_t* l, uint8_t anchoring, int n,
                   list_height_t height, list_draw_t draw, void* ctx)
{
  scroll_set(&l->rol, ROLAGEM_VERTICAL, anchoring);
  l->n = n;
  l->height = height;
  l->draw = draw;
  l->ctx = ctx;
  l->topo = 0;
  l->total = 0;
  l->measurement_ok = false;
  l->everything = true;
  l->window_ant = { 0, 0, 0, 0 };
  l->no_ar = false;
  l->dirty_of = 1; l->dirty_ate = 0;
}

static int somar(const list_t* l)
{
  int t = l->topo;
  for (int i = 0; i < l->n; ++i) { t += l->height(i, l->ctx); }
  return t;
}

void list_items_changed(list_t* l, int n)
{
  const int antes = l->measurement_ok ? l->total : -1;
  l->n = n;
  l->total = somar(l);
  l->measurement_ok = true;
  l->everything = true;

  // READING THE HISTORY WHEN SOMETHING NEW COMES BELOW. In an anchored list
  // end, 'current' counts from bottom: if content grows and 'current' stays
  // same, everything you are reading goes up together. Adding the growth, the
  // that this on screen remains stopped. Pasted into end (current == 0) remains pasted:
  // the new message appears.
  if (l->rol.anchoring == ROLAGEM_FIM && antes >= 0 && l->rol.current > 0 && l->total > antes) {
    l->rol.current += l->total - antes;
    l->rol.no_visor = l->rol.current;
  }
}

void list_item_changed(list_t* l, int i)
{
  if (i < 0) { return; }
  if (l->dirty_of > l->dirty_ate) { l->dirty_of = l->dirty_ate = i; return; }
  if (i < l->dirty_of) { l->dirty_of = i; }
  if (i > l->dirty_ate) { l->dirty_ate = i; }
}

void list_on_start(list_t* l)
{
  scroll_reset(&l->rol);
  l->everything = true;
}

void list_hide(list_t* l)
{
  l->no_ar = false;
  l->rol.anchor = -1;
  l->everything = true;   // reappearing, the window was painted by another screen
}

// Where item 0 begins, in screen coordinates.
static int y_of_first(const list_t* l, const area_t& j)
{
  int y0;
  if (l->rol.anchoring == ROLAGEM_FIM) {
    if (l->total <= j.height) { y0 = j.y + j.height - l->total; }   // little: glued underneath
    else                      { y0 = j.y - (l->total - j.height - l->rol.current); }
  } else {
    y0 = j.y - l->rol.current;
  }
  return y0 + l->topo;
}

static int width_item(const area_t& j) { return j.width - BARRA_MARGEM - BARRA_LARG; }

// Draws the items that intersect 'rec' (already cut by the caller).
static void draw_faixa(list_t* l, lgfx::LovyanGFX& d, const area_t& j, const area_t& rec, uint16_t fundo)
{
  if (rec.width <= 0 || rec.height <= 0) { return; }
  d.fillRect(rec.x, rec.y, rec.width, rec.height, fundo);

  int32_t cx, cy, cl, ca;
  d.getClipRect(&cx, &cy, &cl, &ca);
  d.setClipRect(rec.x, rec.y, rec.width, rec.height);

  const int end = rec.y + rec.height;
  int y = y_of_first(l, j);
  for (int i = 0; i < l->n && y < end; ++i) {
    const int h = l->height(i, l->ctx);
    if (y + h > rec.y) {
      l->draw(d, i, j.x, y, width_item(j), l->ctx);
      // THE CUTTING IS FROM THE LIST, not from the item. Primitive of M5GFX that messes with it
      // (drawWideLine changes to the of line box and then erases it) released
      // the following items by top of header and footer - trace in
      // two ends of window at each step of scrolling.
      d.setClipRect(rec.x, rec.y, rec.width, rec.height);
    }
    y += h;
  }

  d.setClipRect(cx, cy, cl, ca);
}

// These are the items marked by list_item_changed, and only the visible part of them. A
// column of bar stays out: item never draws in it.
static bool draw_sujos(list_t* l, lgfx::LovyanGFX& d, const area_t& j, uint16_t fundo)
{
  if (l->dirty_of > l->dirty_ate) { return false; }
  bool pintou = false;
  int y = y_of_first(l, j);
  for (int i = 0; i < l->n && i <= l->dirty_ate && y < j.y + j.height; ++i) {
    const int h = l->height(i, l->ctx);
    if (i >= l->dirty_of) {
      int y0 = y, y1 = y + h;
      if (y0 < j.y) { y0 = j.y; }
      if (y1 > j.y + j.height) { y1 = j.y + j.height; }
      if (y1 > y0) {
        const area_t rec = { j.x, y0, width_item(j), y1 - y0 };
        draw_faixa(l, d, j, rec, fundo);
        pintou = true;
      }
    }
    y += h;
  }
  l->dirty_of = 1; l->dirty_ate = 0;
  return pintou;
}

bool list_draw(list_t* l, lgfx::LovyanGFX& d, const area_t& j, uint16_t fundo, bool everything)
{
  if (!l->measurement_ok) { list_items_changed(l, l->n); }
  l->no_ar = true;

  // A window changed (the keyboard opened or closed, the screen rotated): everything is new.
  if (j.x != l->window_ant.x || j.y != l->window_ant.y
   || j.width != l->window_ant.width || j.height != l->window_ant.height) {
    l->window_ant = j;
    everything = true;
  }

  // The limit depends on total and of window - and can pull 'current' of iteration.
  if (scroll_limit(&l->rol, l->total, j.height)) { everything = true; }

  everything = everything || l->everything;
  if (everything) {
    draw_faixa(l, d, j, j, fundo);
    scroll_synchronize(&l->rol);
    l->dirty_of = 1; l->dirty_ate = 0;   // the entire window has already covered those marked
  } else if (scroll_pending(&l->rol)) {
    const scroll_pass_t p = scroll_scroll(&l->rol, j, fundo);
    if (p.moved) { draw_faixa(l, d, j, p.exposta, fundo); }
    else            { draw_faixa(l, d, j, j, fundo); everything = true; }
    draw_sujos(l, d, j, fundo);    // after of offset: new positions
  } else {
    return draw_sujos(l, d, j, fundo);   // are values; the bar does not change
  }
  l->everything = false;

  // THE BAR. The offset dragged its pixels along with the content:
  // the entire column is always rewritten - track + dot, or just background when
  // it should not appear (in a list anchored at the end, anchored at the end).
  const area_t col = { j.x + j.width - BARRA_MARGEM, j.y, BARRA_LARG, j.height };
  d.fillRect(col.x, col.y, col.width, col.height, fundo);
  scroll_barra(d, &l->rol, j, fundo, BARRA_MARGEM);
  return true;
}

// -------------------------------------------------------------------------------
// RING
// -------------------------------------------------------------------------------
// A saved window and of TELA; the touch arrives at coordinates of AREA of app.
static bool in_window(const list_t* l, int x, int y, int* ty)
{
  if (!l->no_ar) { return false; }
  const area_t a = system_area();
  const int tx = x + a.x;
  *ty = y + a.y;
  const area_t& j = l->window_ant;
  return tx >= j.x && tx < j.x + j.width && *ty >= j.y && *ty < j.y + j.height;
}

bool list_drag(list_t* l, int x, int y, bool pressed)
{
  int ty = 0;
  const bool dentro = pressed && in_window(l, x, y, &ty) && l->rol.max > 0;
  return scroll_drag(&l->rol, ty, dentro);
}

int list_item_touched(list_t* l, int x, int y, int* dy)
{
  const int afastou = l->rol.distance;
  if (scroll_foi_arraste(&l->rol)) {   // end of drag is not click
    if (diag_touch_enabled()) { ESP_LOGI("lista", "toque recusado como arrasto da lista (%d px)", afastou); }
    return LISTA_ARRASTE;
  }
  int ty = 0;
  if (!in_window(l, x, y, &ty)) { return -1; }
  int yi = y_of_first(l, l->window_ant);
  for (int i = 0; i < l->n; ++i) {
    const int h = l->height(i, l->ctx);
    if (ty >= yi && ty < yi + h) {
      if (dy) { *dy = ty - yi; }
      return i;
    }
    yi += h;
  }
  return -1;
}
