#include "tab5_ui.hpp"
#include "fonts/soft_fonts.hpp"
#include "tab5_glyphs.hpp"
#include "esp_memory_utils.h"   // esp_ptr_external_ram
#include <string.h>

// -------------------------------------------------------------------------------
// SMOOTH FONTS - loaded ONCE, read from flash indefinitely
// -------------------------------------------------------------------------------
// Each VLWfont stores a pointer to its DataWrapper and reads the bitmap of
// each character on demand (seek + read) - therefore, both the wrapper and
// the font object has static lifetime, and changing the font size only
// switches a pointer (nothing is allocated or parsed for each text rendering).
// The metrics table for each font (~9 bytes per glyph) is stored in PSRAM
// during loadFont.
//
// ONLY ONE TASK RENDERS: the wrapper maintains its own read position, and two
// tasks using the same font simultaneously would interfere with each other's
// reads. Currently, only the UI renders text (it owns the composer) - any
// future task that needs to render text must use its own copy.
#define FONTES_MAX 24
static FastFont            s_vlw[2][FONTES_MAX];   // VLW with cache of glyphs (tab5_glyphs)
static lgfx::PointerWrapper s_data[2][FONTES_MAX];
static int                 s_height[2][FONTES_MAX];
static int                 s_n[2] = { 0, 0 };

void tab5_fontes_start(void)
{
  const fonte_suave_t* tab[2] = { FONTES_CORPO, FONTES_NEGRITO };
  const int            n[2]   = { FONTES_CORPO_N, FONTES_NEGRITO_N };
  for (int f = 0; f < 2; ++f) {
    s_n[f] = 0;
    for (int i = 0; i < n[f] && s_n[f] < FONTES_MAX; ++i) {
      const int k = s_n[f];
      s_data[f][k].set(tab[f][i].data, tab[f][i].tamanho);
      if (s_vlw[f][k].loadFont(&s_data[f][k])) {
        s_height[f][k] = tab[f][i].height;
        ++s_n[f];
      }
    }
  }
}

const lgfx::IFont* tab5_fonte_suave(bool bold, int height_px)
{
  const int f = bold ? 1 : 0;
  if (s_n[f] == 0) {   // before tab5_fontes_start (or if memory is unavailable): the old bitmap font
    return bold ? (const lgfx::IFont*)&fonts::FreeSansBold12pt7b : (const lgfx::IFont*)&fonts::FreeSans12pt7b;
  }
  int best = 0;
  for (int i = 1; i < s_n[f]; ++i) {
    if (abs(s_height[f][i] - height_px) < abs(s_height[f][best] - height_px)) { best = i; }
  }
  return &s_vlw[f][best];
}

// Which family the current font belongs to: 0 body, 1 bold, -1 other (title...).
static int family_of(const lgfx::IFont* font)
{
  if (font == &fonts::FreeSans12pt7b)     { return 0; }
  if (font == &fonts::FreeSansBold12pt7b) { return 1; }
  for (int f = 0; f < 2; ++f) {
    for (int i = 0; i < s_n[f]; ++i) { if (font == &s_vlw[f][i]) { return f; } }
  }
  return -1;
}

void tab5_text(lgfx::LovyanGFX& d, float size)
{
  // Font that is neither body nor bold (a title or Font0 that someone
  // left behind): it is the body text that is desired here - no call
  // requests tab5_text on the title (checked). Returning to the
  // body ensures the text remains the same regardless of what was drawn before.
  // (The first version called itself in this case: infinite loop in the status
  // bar, watchdog interface every 5 s.)
  const int f = family_of(d.getFont());
  d.setFont(tab5_fonte_suave(f == 1, (int)(size * 8.0f + 0.5f)));
  d.setTextSize(1);
}

void tab5_fonte_body(lgfx::LovyanGFX& d)          { d.setFont(TAB5_FONTE_CORPO); d.setTextSize(1); }
void tab5_fonte_body_negrito(lgfx::LovyanGFX& d)  { d.setFont(tab5_fonte_suave(true, 29)); d.setTextSize(1); }
void tab5_titulo(lgfx::LovyanGFX& d, int height_px)   { d.setFont(tab5_fonte_suave(true, height_px)); d.setTextSize(1); }

void tab5_botao(lgfx::LovyanGFX& d, int x, int y, int w, int h,
               const char* txt, uint16_t bg,
               uint16_t fg, float text_size)
{
  // Saves and restores the text style. Without this, the middle_center below LEAKS
  // to whoever draws afterward, and all subsequent text stays centered on the x
  // instead of starting there - which pushes long lines off the screen to the
  // left. The same trap already affected tab5_power_mostrar and, before this
  // function existed, four different apps independently fell into it.
  //
  // Copy BY VALUE: getTextStyle() returns a reference to the internal member.
  const lgfx::TextStyle style = d.getTextStyle();

  d.fillRoundRect(x, y, w, h, 12, bg);
  d.setTextDatum(middle_center);
  d.setTextColor(fg, bg);
// BOLD and SIZED TO FIT THE BUTTON. Previously, the text used a
// fixed size requested by the caller (2.0 or 4.0), and a tall button with
// 16 px text looked "empty" and was difficult to read. Now the text occupies
// ~55% of the button's height and only shrinks if it does not fit within
// the width (with 12 px of padding on each side). The caller's
// 'text_size' parameter no longer has any effect: the button determines
// the text size, and all system buttons maintain the same proportions.
  (void)text_size;
  const int margin = 12;
  int size = (h * 55) / 100;
  for (;;) {
    d.setFont(tab5_fonte_suave(true, size));
    d.setTextSize(1);
    if (size <= 12 || (int)d.textWidth(txt) <= w - 2 * margin) { break; }
    size -= 2;
  }
  d.drawString(txt, x + w / 2, y + h / 2);
  tab5_fonte_body(d);

  d.setTextStyle(style);
}

void tab5_celula(lgfx::LovyanGFX& d, int cx, int y0,
                 const char* label, const char* value, uint16_t value_color,
                 const char* subtitle)
{
  const lgfx::TextStyle style = d.getTextStyle();   // see the warning in tab5_botao()

  d.setTextDatum(top_center);

  tab5_fonte_body(d);
  tab5_text(d, 2);
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  d.drawString(label, cx, y0);

  tab5_text(d, 4);
  d.setTextColor(value_color, TFT_BLACK);
  d.drawString(value, cx, y0 + 22);

  if (subtitle && subtitle[0]) {
    tab5_text(d, 2);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString(subtitle, cx, y0 + 66);
  }

  d.setTextStyle(style);
}

// M5GFX's draw_gradient_wedgeline (LGFXBase.cpp), line by line - same
// pixels, same scan optimization (each row stops when it exits the line,
// and the next starts where the previous one entered) - with ONE difference:
// the bounding box is clipped to the current clipping region, instead of
// replacing the clipping region and clearing it at the end.
static float distance_to_segment(float px, float py, float bx, float by)
{
  const float d = (px * bx + py * by) / (bx * bx + by * by);
  const float h = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
  const float dx = px - bx * h, dy = py - by * h;
  return sqrtf(dx * dx + dy * dy);
}

void tab5_line_grossa(lgfx::LovyanGFX& d, float ax, float ay, float bx, float by,
                       float r, uint16_t color)
{
  if (r < 0.0f) { return; }
  if (fabsf(ax - bx) < 0.01f && fabsf(ay - by) < 0.01f) { bx += 0.01f; }   // avoids division by zero
  const float bottom = 1.0f / 32.0f, top = 1.0f - bottom;

  int32_t x0 = (int32_t)floorf(fminf(ax - r, bx - r));
  int32_t x1 = (int32_t)ceilf(fmaxf(ax + r, bx + r));
  int32_t y0 = (int32_t)floorf(fminf(ay - r, by - r));
  int32_t y1 = (int32_t)ceilf(fmaxf(ay + r, by + r));
  int32_t cx, cy, cl, ca;
  d.getClipRect(&cx, &cy, &cl, &ca);
  if (x0 < cx) { x0 = cx; }
  if (y0 < cy) { y0 = cy; }
  if (x1 > cx + cl - 1) { x1 = cx + cl - 1; }
  if (y1 > cy + ca - 1) { y1 = cy + ca - 1; }
  if (x0 > x1 || y0 > y1) { return; }

  int32_t ys = (int32_t)ay;
  if ((ax - r) > (bx - r)) { ys = (int32_t)by; }
  if (ys < y0) { ys = y0; }
  if (ys > y1 + 1) { ys = y1 + 1; }
  const float ar = r + 0.5f;
  const float bax = bx - ax, bay = by - ay;

  d.startWrite();
  for (int pass = 0; pass < 2; ++pass) {
    // 1st pass: from ys to bottom; 2nd pass: from ys-1 to top.
    int32_t xs = x0;
    const int32_t step = pass ? -1 : 1;
    for (int32_t yp = pass ? ys - 1 : ys; pass ? (yp >= y0) : (yp <= y1); yp += step) {
      bool inside = false;
      float alpha = 1.0f;
      for (int32_t xp = xs; xp <= x1; ++xp) {
        if (inside && alpha <= bottom) { break; }
        alpha = ar - distance_to_segment(xp - ax, yp - ay, bax, bay);
        if (alpha <= bottom) { continue; }
        if (!inside) { inside = true; xs = xp; }
        if (alpha > top) { d.drawPixel(xp, yp, color); }
        else             { d.fillRectAlpha(xp, yp, 1, 1, (uint8_t)(alpha * 255.0f), color); }
      }
    }
  }
  d.endWrite();
}

// -------------------------------------------------------------------------------
// SPRITE IN PSRAM
// -------------------------------------------------------------------------------
M5Canvas* tab5_sprite_novo(lgfx::LovyanGFX* parent, int width, int height)
{
  if (width <= 0 || height <= 0) { return nullptr; }
  M5Canvas* c = new M5Canvas(parent);
  c->setPsram(true);
  c->setColorDepth(16);
  // createSprite() may have failed internally (the M5GFX fallback when a PSRAM
  // allocation fails): check WHERE the buffer ended up, not just if it exists.
  if (!c->createSprite(width, height) || !esp_ptr_external_ram(c->getBuffer())) {
    c->deleteSprite();
    delete c;
    return nullptr;
  }
  c->setFont(TAB5_FONTE_CORPO);
  return c;
}

void tab5_sprite_release(M5Canvas** sprite)
{
  if (!sprite || !*sprite) { return; }
  (*sprite)->deleteSprite();
  delete *sprite;
  *sprite = nullptr;
}

// -------------------------------------------------------------------------------
// CUT TO FIT (see tab5_ui.hpp)
// -------------------------------------------------------------------------------
// Continuation byte of a UTF-8 character.
static inline bool utf8_continuation(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

// Largest prefix of 't' (at a character boundary) that measures <= lim. 'n' is the
// length of 't'; the entire 't' does NOT fit (caller has already checked).
static size_t largest_prefix(lgfx::LovyanGFX& d, char* t, size_t n, int lim)
{
  size_t lo = 0, hi = n;   // the prefix of length lo fits; the one of length hi does not
  while (hi - lo > 1) {
    size_t middle = (lo + hi) / 2;
    while (middle > lo && utf8_continuation(t[middle])) { --middle; }
    if (middle == lo) {      // no character start before middle: search after
      middle = (lo + hi) / 2;
      while (middle < hi && utf8_continuation(t[middle])) { ++middle; }
      if (middle >= hi) { break; }   // only one character between lo and hi, and it does not fit
    }
    const char saved = t[middle];
    t[middle] = 0;
    const bool fits = (int)d.textWidth(t) <= lim;
    t[middle] = saved;
    if (fits) { lo = middle; } else { hi = middle; }
  }
  return lo;
}

size_t tab5_cortar_on_caber(lgfx::LovyanGFX& d, char* t, int max_l)
{
  if (!t) { return 0; }
  const size_t n = strlen(t);
  if (n == 0 || (int)d.textWidth(t) <= max_l) { return n; }
  const size_t k = largest_prefix(d, t, n, max_l);
  t[k] = 0;
  return k;
}

size_t tab5_cortar_com_reticencias(lgfx::LovyanGFX& d, char* t, size_t size, int max_l)
{
  if (!t || size < 4) { return t ? strlen(t) : 0; }
  const size_t n = strlen(t);
  if (n == 0 || (int)d.textWidth(t) <= max_l) { return n; }
  size_t k = largest_prefix(d, t, n, max_l - (int)d.textWidth("..."));
  if (k + 4 > size) {   // no space for the three dots: back up by character
    k = size - 4;
    while (k > 0 && utf8_continuation(t[k])) { --k; }
  }
  memcpy(t + k, "...", 4);
  return k + 3;
}
