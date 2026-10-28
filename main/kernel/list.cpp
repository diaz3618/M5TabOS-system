#include "list.hpp"
#include "internal.hpp"   // diag_toque_ligado
#include "esp_log.h"
#include "app.h"   // sistema_area - o touch chega em coordenadas of area

static const int BARRA_MARGEM = 8;   // rolagem_barra desenha 4px a 8px of borda
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

  // LENDO O HISTORICO QUANDO CHEGA ALGO NOVO EMBAIXO. Numa lista anchored no
  // end, 'current' conta a partir of bottom: se o content cresce e 'current' stays
  // igual, everything o que voce esta lendo sobe junto. Somando o crescimento, o
  // que esta in screen stays parado. Colado no end (current == 0) continua colado:
  // a message nova aparece.
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
  l->everything = true;   // voltando a aparecer, a window foi pintada por outra screen
}

// Onde comeca o item 0, em coordenadas of screen.
static int y_of_first(const list_t* l, const area_t& j)
{
  int y0;
  if (l->rol.anchoring == ROLAGEM_FIM) {
    if (l->total <= j.height) { y0 = j.y + j.height - l->total; }   // pouca coisa: colada embaixo
    else                      { y0 = j.y - (l->total - j.height - l->rol.current); }
  } else {
    y0 = j.y - l->rol.current;
  }
  return y0 + l->topo;
}

static int width_item(const area_t& j) { return j.width - BARRA_MARGEM - BARRA_LARG; }

// Desenha os items que cruzam 'rec' (ja recortado pelo chamador).
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
      // O RECORTE E DA LISTA, nao of item. Primitivo of M5GFX que mexe nele
      // (drawWideLine troca pela caixa of line e depois o apaga) soltava
      // os items seguintes por top of cabecalho e of rodape - rastro nas
      // duas pontas of window a cada passo of rolagem.
      d.setClipRect(rec.x, rec.y, rec.width, rec.height);
    }
    y += h;
  }

  d.setClipRect(cx, cy, cl, ca);
}

// So os items marcados por lista_item_mudou, e so a parte visivel deles. A
// coluna of barra stays of fora: o item nunca desenha nela.
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

  // A window changed (o keyboard abriu ou fechou, a screen girou): everything of novo.
  if (j.x != l->window_ant.x || j.y != l->window_ant.y
   || j.width != l->window_ant.width || j.height != l->window_ant.height) {
    l->window_ant = j;
    everything = true;
  }

  // O limit depende of total e of window - e pode puxar 'current' of iteration.
  if (scroll_limit(&l->rol, l->total, j.height)) { everything = true; }

  everything = everything || l->everything;
  if (everything) {
    draw_faixa(l, d, j, j, fundo);
    scroll_synchronize(&l->rol);
    l->dirty_of = 1; l->dirty_ate = 0;   // a window inteira ja cobriu os marcados
  } else if (scroll_pending(&l->rol)) {
    const scroll_pass_t p = scroll_scroll(&l->rol, j, fundo);
    if (p.moved) { draw_faixa(l, d, j, p.exposta, fundo); }
    else            { draw_faixa(l, d, j, j, fundo); everything = true; }
    draw_sujos(l, d, j, fundo);    // depois of offset: posicoes novas
  } else {
    return draw_sujos(l, d, j, fundo);   // so valores; a barra nao changed
  }
  l->everything = false;

  // A BARRA. O offset arrastou os pixels dela junto com o content:
  // a coluna e sempre reescrita inteira - track + bolinha, ou so fundo when
  // ela nao deve aparecer (numa lista anchored no end, encostada no end).
  const area_t col = { j.x + j.width - BARRA_MARGEM, j.y, BARRA_LARG, j.height };
  d.fillRect(col.x, col.y, col.width, col.height, fundo);
  scroll_barra(d, &l->rol, j, fundo, BARRA_MARGEM);
  return true;
}

// ---------------------------------------------------------------------------
// TOQUE
// ---------------------------------------------------------------------------
// A window guardada e of TELA; o touch chega em coordenadas of AREA of app.
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
  if (scroll_foi_arraste(&l->rol)) {   // end of arraste nao e click
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
