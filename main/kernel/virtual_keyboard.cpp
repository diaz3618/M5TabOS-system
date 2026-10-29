#include "virtual_keyboard.hpp"
#include "tab5_screen.hpp"
#include "tab5_ui.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tab5_time.hpp"   // tab5_now_ms
#include "tab5_keyboard.hpp" // times of repetition
#include <stdlib.h>
#include <string.h>

// THREE PAGES, same geometry (10/10/9/7 keys): switch of page just change
// labels, nothing moves out of place. Each key is ONE UTF-8 character.
// 0 letters: the row of numbers always remains (password, PIN and time without switch
// of page).
// 1 "?123": punctuation and pairs - parentheses, braces, brackets, signs.
// 2 "=\<": the rest of ASCII and the accents of Portuguese (the system font
// has these and not much else - see tools/generate_fonts.py). The
// accents follow the shift, like letters.
static const int N_FILA[TECVIRT_FILAS] = { 10, 10, 9, 7 };
static const char* const PAGINA[3][TECVIRT_FILAS] = {
  { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" },
  { "1234567890", "@#$%&-+()/", "*\"':;!?_=", "[]{}<>\\" },
  { "1234567890", "áàâãéêíóôõ", "úç°ºª|~^`", "•…«»×÷±" },
};

#define FILAS   TECVIRT_FILAS

// -------------------------------------------------------------------------------
// MEASUREMENTS BY GUIDANCE
// -------------------------------------------------------------------------------
// Laying down (1280 of width) the key has ~120 px of width and 52 of height: o
// keyboard and bottom are wide, and there is little height left on screen. Upright and vice versa:
// ten keys at 720 px gives only ~67 px of width, and the screen has a height of plenty.
// Then, standing, the key grows UP (68 px, almost square), the gap
// shrinks a little to return the keys width, and the letter stays bigger. O
// entire keyboard goes up ~80 px - what the standing conversation has to give.
static bool em_pe(const area_t& a) { return a.width < a.height; }
static int  alt_t(const area_t& a)    { return em_pe(a) ? 68 : 52; }   // height of each key
static int  vao(const area_t& a)      { return em_pe(a) ? 5 : 6; }
static int  px_letra(const area_t& a) { return em_pe(a) ? 34 : 30; }   // font of letters and numbers

int virtual_keyboard_height(const area_t& a)
{
  return vao(a) + (FILAS + 1) * (alt_t(a) + vao(a));
}

// -------------------------------------------------------------------------------
// UTF-8 AND SHIFT
// -------------------------------------------------------------------------------
int virtual_keyboard_utf8(int cp, char out[4])
{
  if (cp < 0x80)    { out[0] = (char)cp; return 1; }
  if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

// The codepoint of column 'col' of a row (UTF-8). 0 = out of it.
static int cp_em(const char* s, int col)
{
  const unsigned char* u = (const unsigned char*)s;
  for (int k = 0; *u; ++k) {
    int cp, n;
    if      (u[0] < 0x80)           { cp = u[0]; n = 1; }
    else if ((u[0] & 0xE0) == 0xC0) { cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); n = 2; }
    else if ((u[0] & 0xF0) == 0xE0) { cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F); n = 3; }
    else                            { cp = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F); n = 4; }
    if (k == col) { return cp; }
    u += n;
  }
  return 0;
}

// Letter (ASCII or Latin-1 lower/upper case)? And her capital letter.
static bool eh_letra(int cp)
{
  return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')
      || (cp >= 0xC0 && cp <= 0xFF && cp != 0xD7 && cp != 0xF7);
}
static int maiuscula(int cp)
{
  if (cp >= 'a' && cp <= 'z') { return cp - 32; }
  if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) { return cp - 0x20; }
  return cp;
}

// What the key of character types now (page and shift applied).
static int cp_key(const virtual_keyboard_t* t, int fila, int col)
{
  if (fila < 0 || fila >= FILAS || col < 0 || col >= N_FILA[fila]) { return 0; }
  const int cp = cp_em(PAGINA[t->pagina][fila], col);
  return t->shift != TECVIRT_SHIFT_NAO ? maiuscula(cp) : cp;
}

// -------------------------------------------------------------------------------
// SPECIAL KEYS
// -------------------------------------------------------------------------------
// As in the keyboard of Android: shift and erase at the ends of row of "zxcv",
// and below the exchange of page, comma, space, period, SEND and close. color,
// what it does is the width in one place: o drawing complete, o of a key
// isolated and touch must agree. In the command line the width goes
// in HALF units of 1/10 of width (20 in total); shift and erase occupy
// the remainder of row of 7 keys (1.5 units each).
//
// ICONS DRAWN, NOT TYPETED: codes < 0x20 become drawing in
// paint_key (shift released/one letter/locked and erase).
#define ICO_SHIFT  "\x01"
#define ICO_SHIFT1 "\x02"
#define ICO_CAPS   "\x03"
#define ICO_APAGA  "\x04"
enum { E_SHIFT, E_APAGA, E_MODO, E_VIRG, E_ESPACO, E_PONTO, E_ENVIAR, E_FECHAR, NESP };
#define ID_ESP(i) (64 + (i))
#define CMD0 E_MODO   // the first special of command
typedef struct { uint16_t color; int acao; int meias; } esp_t;
static const esp_t ESP[NESP] = {
  { TFT_LIGHTGREY,   0,              0 },   // shift / exchange of page of symbols (internal)
  { TFT_ORANGE,      TECVIRT_APAGAR, 0 },
  { TFT_CYAN,        0,              3 },   // ?123 / ABC (internal)
  { TFT_LIGHTGREY,   ',',            2 },
  { TFT_LIGHTGREY,   ' ',            8 },
  { TFT_LIGHTGREY,   '.',            2 },
  { TFT_GREENYELLOW, TECVIRT_ENVIAR, 3 },
  { TFT_RED,         TECVIRT_FECHAR, 2 },
};

// The label of special in the current state.
static const char* rotulo_esp(const virtual_keyboard_t* t, int i)
{
  switch (i) {
    case E_SHIFT:
      if (t->pagina == 1) { return "=\\<"; }
      if (t->pagina == 2) { return "?123"; }
      return t->shift == TECVIRT_SHIFT_TRAVA ? ICO_CAPS : t->shift == TECVIRT_SHIFT_UMA ? ICO_SHIFT1 : ICO_SHIFT;
    case E_APAGA:  return ICO_APAGA;
    case E_MODO:   return t->pagina ? "ABC" : "?123";
    case E_VIRG:   return ",";
    case E_ESPACO: return "space";
    case E_PONTO:  return ".";
    case E_ENVIAR: return "SEND";
    default:       return "X";
  }
}

static uint16_t color_esp(const virtual_keyboard_t* t, int i)
{
  if (i == E_SHIFT && t->pagina == 0 && t->shift != TECVIRT_SHIFT_NAO) { return TFT_CYAN; }   // lit = valid
  return ESP[i].color;
}

// -------------------------------------------------------------------------------
// GEOMETRY - calculated and not tabulated: the width changes with the rotation.
// -------------------------------------------------------------------------------
static int topo_of(const area_t& a) { return a.y + a.height - virtual_keyboard_height(a); }
static int y_fila(const area_t& a, int f) { return topo_of(a) + vao(a) + f * (alt_t(a) + vao(a)); }
static int larg_key(const area_t& a) { return (a.width - 2 * vao(a) - vao(a) * 9) / 10; }   // for the longest row
static int recuo_fila(const area_t& a, int f)
{
  const int u = larg_key(a) + vao(a);
  return (a.width - 2 * vao(a) - N_FILA[f] * u + vao(a)) / 2;
}

static bool key_em(const area_t& a, int fila, int col, int* x, int* y, int* l)
{
  if (fila < 0 || fila >= FILAS || col < 0 || col >= N_FILA[fila]) { return false; }
  *l = larg_key(a);
  *x = a.x + vao(a) + recuo_fila(a, fila) + col * (*l + vao(a));
  *y = y_fila(a, fila);
  return true;
}

static bool especial_em(const area_t& a, int i, int* x, int* y, int* l)
{
  if (i < 0 || i >= NESP) { return false; }
  const int total = a.width - 2 * vao(a);
  if (i == E_SHIFT || i == E_APAGA) {
    const int r = recuo_fila(a, FILAS - 1);
    const int end_letras = r + N_FILA[FILAS - 1] * (larg_key(a) + vao(a));
    *y = y_fila(a, FILAS - 1);
    if (i == E_SHIFT) { *x = a.x + vao(a);              *l = r - vao(a); }
    else              { *x = a.x + vao(a) + end_letras; *l = total - end_letras; }
    return true;
  }
  int start = 0;
  for (int k = CMD0; k < i; ++k) { start += ESP[k].meias; }
  *x = a.x + vao(a) + (start * total) / 20;
  *y = y_fila(a, FILAS);
  *l = ((start + ESP[i].meias) * total) / 20 - (start * total) / 20 - vao(a);
  return true;
}

static bool rectangle(const area_t& a, int id, int* x, int* y, int* l)
{
  if (id >= 64) { return especial_em(a, id - 64, x, y, l); }
  if (id < 0) { return false; }
  return key_em(a, id / 16, id % 16, x, y, l);
}

// THE TARGET OF A KEY (where the touch applies to it) is greater than the drawing:
// - the gap between keys and someone: each responds up to half of it,
// as in keyboard of cell phone;
// - at the ends of the recessed rows ("asdf..."), half an extra key to
// out: the recoil was neutral, and the finger that aims at the "a" and falls a little to
// his left wants the "a", not anything. Shift and erase go to the edge;
// - the command line goes from edge to edge and to bottom without limit (a
// track of navigation stuck delivers touch in last line of area).
static const int ALVO_SEM_FIM = 1 << 14;

static bool alvo_of(const area_t& a, int id, int* x0, int* y0, int* x1, int* y1)
{
  const int f = (vao(a) + 1) / 2;
  int x, y, l;
  if (!rectangle(a, id, &x, &y, &l)) { return false; }
  *x0 = x - f;
  *x1 = x + l + f;
  *y0 = y - f;
  *y1 = y + alt_t(a) + f;
  if (id >= 64) {
    const int i = id - 64;
    if (i == E_SHIFT || i == CMD0) { *x0 = a.x; }
    if (i == E_APAGA || i == E_FECHAR) { *x1 = a.x + a.width; }
    if (i >= CMD0) { *y1 = ALVO_SEM_FIM; }
    return true;
  }
  const int fi = id / 16, c = id % 16;
  if (fi < FILAS - 1) {
    if (c == 0)              { *x0 -= l / 2; }
    if (c == N_FILA[fi] - 1) { *x1 += l / 2; }
  }
  return true;
}

static bool no_alvo(const area_t& a, int id, int xs, int ys, int folga)
{
  int x0, y0, x1, y1;
  if (!alvo_of(a, id, &x0, &y0, &x1, &y1)) { return false; }
  return xs >= x0 - folga && xs < x1 + folga && ys >= y0 - folga && ys < y1 + folga;
}

// HYSTERESIS: the finger that is already in a key only passes to the neighboring one when
// enters HISTERESE_PX in it. On the exact border, the tremor of finger (and the of
// GT911) changed the key of one sample for another - and the one that counted was the of
// last sample before release, in the draw. 'current' = the key in which the
// finger was (TECVIRT_NONE for new one touch).
static const int HISTERESE_PX = 8;

static int key_no_ponto(const area_t& a, int xs, int ys, int current)
{
  if (current != TECVIRT_NENHUMA && no_alvo(a, current, xs, ys, HISTERESE_PX)) { return current; }
  for (int fi = 0; fi < FILAS; ++fi) {
    for (int c = 0; c < N_FILA[fi]; ++c) {
      if (no_alvo(a, fi * 16 + c, xs, ys, 0)) { return fi * 16 + c; }
    }
  }
  for (int i = 0; i < NESP; ++i) {
    if (no_alvo(a, ID_ESP(i), xs, ys, 0)) { return ID_ESP(i); }
  }
  return TECVIRT_NENHUMA;
}

static bool eh_erase(int id) { return id == ID_ESP(E_APAGA); }

// -------------------------------------------------------------------------------
// DRAWING - are primitives and the font of system (cache of glyphs): none
// saved image. A ready-made keyboard image would cost ~420 KB of PSRAM per
// state to save a few months on a paint that only happens on open and on
// switch shift/page; In everyday life, only 1-2 keys are repainted per touch.
// -------------------------------------------------------------------------------

// The font size for each special key, once per width: the LARGEST label it
// can show ("SEND", "space", "?123") steps down by 2 at a time until it
// fits (5 px of breathing room on each side).
static void medir(virtual_keyboard_t* t, lgfx::LovyanGFX& d)
{
  static const char* const VARIANTES[NESP][2] = {
    { "=\\<", "?123" }, { "", "" }, { "?123", "ABC" }, { ",", "" },
    { "space", "" }, { ".", "" }, { "SEND", "" }, { "X", "" },
  };
  if (t->med_larg == t->a.width) { return; }
  t->med_larg = t->a.width;
  for (int i = 0; i < NESP; ++i) {
    int x, y, l;
    especial_em(t->a, i, &x, &y, &l);
    int px = px_letra(t->a);
    for (; px > 14; px -= 2) {
      d.setFont(tab5_fonte_suave(true, px));
      int w = 0;
      for (int v = 0; v < 2; ++v) { const int wv = (int)d.textWidth(VARIANTES[i][v]); if (wv > w) { w = wv; } }
      if (w <= l - 10) { break; }
    }
    t->px_esp[i] = (uint8_t)px;
  }
}

// Arrow of shift: full when valid (one letter or locked), only the outline
// when released; locked gains the bar below.
static void icon_shift(lgfx::LovyanGFX& d, int cx, int cy, char tipo)
{
  const int oy = (tipo == ICO_CAPS[0]) ? cy - 3 : cy;
  if (tipo == ICO_SHIFT[0]) {
    for (int o = 0; o < 2; ++o) {
      d.drawTriangle(cx, oy - 15 + o, cx - 15 + o, oy + 1 - o, cx + 15 - o, oy + 1 - o, TFT_BLACK);
      d.drawRect(cx - 6 + o, oy + 1 - o, 12 - 2 * o, 13, TFT_BLACK);
    }
    return;
  }
  d.fillTriangle(cx, oy - 15, cx - 15, oy + 1, cx + 15, oy + 1, TFT_BLACK);
  d.fillRect(cx - 6, oy + 1, 12, 13, TFT_BLACK);
  if (tipo == ICO_CAPS[0]) { d.fillRect(cx - 13, oy + 18, 26, 4, TFT_BLACK); }
}

// Delete: body with tip to the left and one X hollowed out in color of key.
static void icon_erase(lgfx::LovyanGFX& d, int cx, int cy, uint16_t color)
{
  d.fillTriangle(cx - 24, cy, cx - 9, cy - 14, cx - 9, cy + 14, TFT_BLACK);
  d.fillRect(cx - 9, cy - 14, 33, 29, TFT_BLACK);
  for (int o = -1; o <= 1; ++o) {
    d.drawLine(cx + 1 + o, cy - 6, cx + 13 + o, cy + 6, color);
    d.drawLine(cx + 13 + o, cy - 6, cx + 1 + o, cy + 6, color);
  }
}

static void paint_key(const virtual_keyboard_t* t, lgfx::LovyanGFX& d, int id, bool acesa)
{
  int x, y, l;
  if (!rectangle(t->a, id, &x, &y, &l)) { return; }
  char letra[5];
  const char* txt;
  uint16_t color;
  int px;
  if (id >= 64) {
    txt = rotulo_esp(t, id - 64);
    color = color_esp(t, id - 64);
    px  = t->px_esp[id - 64];
  } else {
    letra[virtual_keyboard_utf8(cp_key(t, id / 16, id % 16), letra)] = 0;
    txt = letra;
    color = TFT_LIGHTGREY;
    px  = px_letra(t->a);
  }
  // Lit = BLUE, not "the same lighter color": under the finger only the edge
  // appears, and a DIFFERENT color is what you see in it.
  if (acesa) { color = TFT_SKYBLUE; }
  d.fillRoundRect(x, y, l, alt_t(t->a), 8, color);

  const int cx = x + l / 2, cy = y + alt_t(t->a) / 2;
  if ((unsigned char)txt[0] < 0x20) {
    if (txt[0] == ICO_APAGA[0]) { icon_erase(d, cx, cy, color); }
    else                        { icon_shift(d, cx, cy, txt[0]); }
    return;
  }
  d.setFont(tab5_fonte_suave(true, px));
  d.setTextColor(TFT_BLACK, color);
  d.drawString(txt, cx, cy);
}

void virtual_keyboard_paint(virtual_keyboard_t* t, lgfx::LovyanGFX& d, bool complete)
{
  if (!t->open) { return; }
  const bool everything = complete || t->dirty;
  const bool rot  = !everything && t->rotulos;
  if (!everything && !rot && t->key_sob == t->key_pinta) { return; }   // nothing changed: it doesn't even touch on screen

  const lgfx::TextStyle estilo = d.getTextStyle();
  const lgfx::IFont* fonte = d.getFont();
  medir(t, d);
  d.setTextSize(1);
  d.setTextDatum(middle_center);

  if (everything || rot) {
    // The background is only cleared when the keyboard appears or changes location; in exchange
    // of shift/page the keys are opaque, the corners are already black, and
    // repainting them all is simpler (and almost as cheap) as choosing which ones.
    if (everything) { tab5_screen_clear(t->a.x, topo_of(t->a), t->a.width, virtual_keyboard_height(t->a), TFT_BLACK); }
    for (int f = 0; f < FILAS; ++f) {
      for (int c = 0; c < N_FILA[f]; ++c) { paint_key(t, d, f * 16 + c, f * 16 + c == t->key_sob); }
    }
    for (int i = 0; i < NESP; ++i) { paint_key(t, d, ID_ESP(i), ID_ESP(i) == t->key_sob); }
    t->dirty = t->rotulos = false;
  } else {
    // Return of key: no maximum two - the one finger left and the one he found.
    if (t->key_pinta != TECVIRT_NENHUMA) { paint_key(t, d, t->key_pinta, false); }
    if (t->key_sob   != TECVIRT_NENHUMA) { paint_key(t, d, t->key_sob, true); }
  }
  t->key_pinta = t->key_sob;

  d.setFont(fonte);
  d.setTextStyle(estilo);
}

// -------------------------------------------------------------------------------
// STATUS: shift, page, automatic capitalization
// -------------------------------------------------------------------------------
static void mudar(virtual_keyboard_t* t, uint8_t shift, uint8_t pagina)
{
  if (shift == t->shift && pagina == t->pagina) { return; }
  t->shift = shift;
  t->pagina = pagina;
  t->rotulos = true;
  system_request_redraw();
}

void virtual_keyboard_phrase(virtual_keyboard_t* t, bool enabled) { t->phrase = enabled; }

void virtual_keyboard_context(virtual_keyboard_t* t, const char* text, int cursor)
{
  if (!t->phrase || !t->open || t->shift == TECVIRT_SHIFT_TRAVA) { return; }
  int i = cursor;
  const int n = text ? (int)strlen(text) : 0;
  if (i > n) { i = n; }
  if (i < 0) { i = 0; }
  bool espaco = false;
  while (i > 0 && text[i - 1] == ' ') { --i; espaco = true; }
  const bool start = i == 0 || (espaco && (text[i - 1] == '.' || text[i - 1] == '!' || text[i - 1] == '?'));
  mudar(t, start ? TECVIRT_SHIFT_UMA : TECVIRT_SHIFT_NAO, t->pagina);
}

bool virtual_keyboard_edit(char* buf, size_t cap, int acao, bool ascii_only)
{
  size_t n = strlen(buf);
  if (acao == TECVIRT_APAGAR) {
    while (n > 0) {
      const unsigned char b = (unsigned char)buf[--n];
      if ((b & 0xC0) != 0x80) { break; }   // continuation of UTF-8: remove the entire character
    }
    buf[n] = 0;
    return true;
  }
  if (acao <= 0) { return false; }
  if (acao < 0x20 || (ascii_only && acao >= 0x7F)) { return true; }
  char u[4];
  const int k = virtual_keyboard_utf8(acao, u);
  if (n + k < cap) { memcpy(buf + n, u, k); buf[n + k] = 0; }
  return true;
}

// -------------------------------------------------------------------------------
// LIFE CYCLE
// -------------------------------------------------------------------------------
static void release_fingers(virtual_keyboard_t* t)
{
  for (int k = 0; k < SYSTEM_MAX_TOUCHES; ++k) {
    auto& dd = t->fingers[k];
    dd.id = -1;
    dd.key = dd.firme = dd.cand = TECVIRT_NENHUMA;
    dd.cand_n = 0;
    dd.eco = dd.repetiu = false;
  }
}

void virtual_keyboard_open(virtual_keyboard_t* t)
{
  if (t->open) { return; }
  t->open = true;
  t->shift = TECVIRT_SHIFT_NAO;
  t->pagina = 0;
  t->dirty = true;
  t->rotulos = false;
  t->livre = 0;
  t->key_sob = t->key_pinta = t->ultima_key = TECVIRT_NENHUMA;
  t->released.key = TECVIRT_NENHUMA;
  release_fingers(t);
  system_lock_navigation(true);   // HOME stays just below key of space
}

void virtual_keyboard_close(virtual_keyboard_t* t)
{
  if (!t->open) { return; }
  t->open = false;
  t->key_sob = t->key_pinta = TECVIRT_NENHUMA;
  release_fingers(t);
  system_lock_navigation(false);
}

void virtual_keyboard_position(virtual_keyboard_t* t, const area_t& a)
{
  if (a.x != t->a.x || a.y != t->a.y || a.width != t->a.width || a.height != t->a.height) {
    t->a = a;
    t->dirty = true;
  }
}

bool virtual_keyboard_contains(const virtual_keyboard_t* t, int xs, int ys)
{
  return t->open && ys >= topo_of(t->a) && xs >= t->a.x && xs < t->a.x + t->a.width;
}

// -------------------------------------------------------------------------------
// RING
// -------------------------------------------------------------------------------

// THE FINGER THAT OPENED THE KEYBOARD DOESN'T TYPE. The release of touch that opened (or
// a delayed sample of GT911 right after it) falls on a key and the text
// was born with one letter. Until the screen runs out of fingers for two samples
// followed by open, nothing responds (level and click).
static const int LIVRE_AMOSTRAS = 2;

static bool fingers_ativos(const virtual_keyboard_t* t)
{
  for (int k = 0; k < SYSTEM_MAX_TOUCHES; ++k) { if (t->fingers[k].id >= 0) { return true; } }
  return false;
}

// Confirms a key (with the brake against rebound). Shift and page switching
// stay inside; the rest goes to the of app list. Returns the action, or 0.
//
// SHIFT LIKE ON A CELL PHONE: one touch sets to ONE letter (it exits capital and the
// shift released alone); another touch until SHIFT_DUPLO_MS locks (caps lock);
// one touch stuck released. On the pages of symbols the same key alternates
// between the two pages. SPACE on a page of symbols iteration the letters, and
// one accent (page 2) too: the word continues in letters.
static int confirm(virtual_keyboard_t* t, int key)
{
  static const uint32_t REPIQUE_MS     = 45;
  static const uint32_t SHIFT_DUPLO_MS = 400;
  if (key == TECVIRT_NENHUMA) { return 0; }
  const uint32_t now = tab5_now_ms();
  // DELETE and the key is hit in a burst at the same point: without brake of repeat.
  if (!eh_erase(key) && key == t->ultima_key && now - t->key_ms < REPIQUE_MS) { return 0; }
  t->key_ms = now;
  t->ultima_key = (int16_t)key;

  if (key == ID_ESP(E_SHIFT)) {
    if (t->pagina) { mudar(t, t->shift, t->pagina == 1 ? 2 : 1); return 0; }
    uint8_t s = TECVIRT_SHIFT_NAO;
    if (t->shift == TECVIRT_SHIFT_NAO)                                   { s = TECVIRT_SHIFT_UMA; }
    else if (t->shift == TECVIRT_SHIFT_UMA && now - t->shift_ms < SHIFT_DUPLO_MS) { s = TECVIRT_SHIFT_TRAVA; }
    t->shift_ms = now;
    mudar(t, s, 0);
    return 0;
  }
  if (key == ID_ESP(E_MODO)) { mudar(t, t->shift, t->pagina ? 0 : 1); return 0; }

  int ac;
  if (key >= 64) {
    if (key - 64 >= NESP) { return 0; }
    ac = ESP[key - 64].acao;
  } else {
    ac = cp_key(t, key / 16, key % 16);
  }
  if (ac > 0) {
    // After of character: shift of A letter released; space and accent return the letters.
    const uint8_t s = t->shift == TECVIRT_SHIFT_UMA ? (uint8_t)TECVIRT_SHIFT_NAO : t->shift;
    const bool iteration = t->pagina && (ac == ' ' || (t->pagina == 2 && eh_letra(ac)));
    mudar(t, s, iteration ? 0 : t->pagina);
  }
  return ac;
}

// ONE FINGER PER KEY, EACH WITH ITS OWN RELEASE.
//
// The key lights up under the finger and is valid in RELEASE of that finger - of for
// slide to the right before release. Quickly typing the next finger
// drops before the previous raise: each finger (id of controller) has its own
// key, and the letters come out in the order in which the fingers are released.
//
// THE KEY THAT IS WORTH IS THE FIRM KEY, not the one of the last sample. When raising, the area of
// contact shrinks and its center moves a few pixels - the last sample before
// of release would catch the neighbor and the lyrics would come out wrong. The descending finger fixes the
// key in hour; slipping to another just change after TECLA_FIRME
// samples in a row (~40 ms): the correction of those who slip on purpose
// is still valid, the trope of release is not. The key is lit and always firm
// - what is lit and what will be typed.
//
// THE LETTER COMES OUT ON THE FIRST SAMPLE WITHOUT THE FINGER. The GT911 sometimes loses one finger
// for a sample, and his iteration would become double lyrics. The iteration is recognized
// so it is - the same key, the same point, IN THE FOLLOWING SAMPLE the
// release - is marked as ECO: lights up, but does not type again. The window and
// counted in samples: the interval between samples varies with the drawing (20 to
// 45 ms), and one limit in months was sometimes short, sometimes long. ECO_MS_MAX and the
// ceiling: in a slow iteration (heavy drawing), "the next sample" may arrive
// late enough to be a new hit of truth at the same point.
// Slipped to another key, stops being echo.
//
// THE ECHO IS NOT WORTHY TO ERASE IT, and it never chains. Shake quickly in the same
// point, the erase rose and fell within the window of echo: the beat was
// swallowed, and her release became the reference of the following - a gust
// whole thing disappeared. Only the release that TYPE serves as a reference, and the erase
// never and echo.
//
// DELETE SAFE REPEAT, as on any keyboard: after of REP_INICIO_MS
// with your finger stopped on it, erases one every REP_MS.
int virtual_keyboard_level(virtual_keyboard_t* t, int* actions, int max)
{
  static const int      SALTO_PX      = 45;
  static const uint32_t ECO_AMOSTRAS  = 1;
  static const uint32_t ECO_MS_MAX    = 60;   // and never more than this (slow iteration)
  static const uint8_t  TECLA_FIRME   = 2;
  // Same times as physical keyboard (hal/tab5_keyboard.hpp).
  static const uint32_t REP_INICIO_MS = TAB5_KEY_REPEAT_START_MS;
  static const uint32_t REP_MS        = TAB5_KEY_REPEAT_MS;
  if (!t->open) { return 0; }
  ++t->sample;
  system_touch_t q[SYSTEM_MAX_TOUCHES];
  const int n = system_touches(q, SYSTEM_MAX_TOUCHES);
  const area_t ap = system_area();
  // Finger in range of keyboard: the touch is his, the release does not become a click
  // nothing (see system_consume_toque). Including lack of opening.
  for (int i = 0; i < n; ++i) {
    if (virtual_keyboard_contains(t, ap.x + q[i].x, ap.y + q[i].y)) { system_consume_touch(); break; }
  }
  if (t->livre < LIVRE_AMOSTRAS) {
    if (n) { t->livre = 0; } else { ++t->livre; }
    release_fingers(t);
    return 0;
  }

  auto key_of = [&](const system_touch_t& p, int current) {
    return (int16_t)key_no_ponto(t->a, ap.x + p.x, ap.y + p.y, current);
  };
  int in = 0;
  auto anotar = [&](int ac) { if (ac && in < max) { actions[in++] = ac; } };
  const int16_t antes = t->key_sob;
  const uint32_t now = tab5_now_ms();

  // 1) Fingers that were already there: follow the key under them, or let go.
  for (int k = 0; k < SYSTEM_MAX_TOUCHES; ++k) {
    auto& dd = t->fingers[k];
    if (dd.id < 0) { continue; }
    int achou = -1;
    for (int i = 0; i < n; ++i) { if (q[i].id == dd.id) { achou = i; break; } }
    if (achou >= 0) {
      const int16_t sob = key_of(q[achou], dd.key);
      const int dx = q[achou].x - dd.x, dy = q[achou].y - dd.y;
      if (sob != dd.key && (abs(dx) > SALTO_PX || abs(dy) > SALTO_PX)) {
        // THE CONTROLLER REUSES THE ID: finger goes up and next goes down
        // within a sample, and for the system and the "same finger" that skipped
        // of key. A real finger is no more than ~45 px in a sample;
        // one bigger jump and one new touch - the previous key is valid and the new one
        // starts strong.
        if (!dd.eco && !dd.repetiu) { anotar(confirm(t, dd.firme)); }
        dd.firme = sob;
        dd.cand = TECVIRT_NENHUMA; dd.cand_n = 0;
        dd.since = dd.rep_ms = now;
        dd.eco = dd.repetiu = false;
      } else if (sob == dd.firme) {
        dd.cand = TECVIRT_NENHUMA; dd.cand_n = 0;      // returned: the deviation does not count
      } else {
        if (sob == dd.cand) { if (dd.cand_n < 255) { ++dd.cand_n; } }
        else                { dd.cand = sob; dd.cand_n = 1; }
        if (dd.cand_n >= TECLA_FIRME) {
          dd.firme = sob;
          dd.cand = TECVIRT_NENHUMA; dd.cand_n = 0;
          dd.eco = dd.repetiu = false;
          dd.rep_ms = now;
        }
      }
      dd.key = sob;
      dd.x = q[achou].x; dd.y = q[achou].y;
      // Repetition of DELETE insured.
      if (eh_erase(dd.firme) && now - dd.rep_ms >= (dd.repetiu ? REP_MS : REP_INICIO_MS)) {
        anotar(confirm(t, dd.firme));
        dd.repetiu = true;
        dd.rep_ms = now;
      }
    } else {
      const bool digita = !dd.eco && !dd.repetiu;
      if (digita) { anotar(confirm(t, dd.firme)); }
      // Only the release you typed is a reference of echo - one echo never generates another.
      t->released = { digita ? dd.firme : (int16_t)TECVIRT_NENHUMA, dd.x, dd.y, t->sample, now };
      dd.id = -1;
      dd.key = dd.firme = dd.cand = TECVIRT_NENHUMA;
      dd.cand_n = 0;
      dd.eco = dd.repetiu = false;
    }
  }
  // 2) New fingers: the key under the point of press_start is already firm.
  for (int i = 0; i < n; ++i) {
    bool conhecido = false;
    for (int k = 0; k < SYSTEM_MAX_TOUCHES; ++k) { if (t->fingers[k].id == q[i].id) { conhecido = true; break; } }
    if (conhecido) { continue; }
    for (int k = 0; k < SYSTEM_MAX_TOUCHES; ++k) {
      auto& dd = t->fingers[k];
      if (dd.id >= 0) { continue; }
      dd.id = (int8_t)q[i].id;
      dd.key = dd.firme = key_of(q[i], TECVIRT_NENHUMA);
      dd.cand = TECVIRT_NENHUMA; dd.cand_n = 0;
      dd.x = q[i].x; dd.y = q[i].y;
      dd.since = dd.rep_ms = now;
      dd.repetiu = false;
      dd.eco = dd.firme != TECVIRT_NENHUMA && dd.firme == t->released.key
            && t->sample - t->released.sample <= ECO_AMOSTRAS && now - t->released.ms <= ECO_MS_MAX
            && !eh_erase(dd.firme)
            && abs(dd.x - t->released.x) <= SALTO_PX && abs(dd.y - t->released.y) <= SALTO_PX;
      break;
    }
  }
  // 3) The key is lit and the firm finger comes down last.
  int recente = -1;
  for (int k = 0; k < SYSTEM_MAX_TOUCHES; ++k) {
    if (t->fingers[k].id < 0) { continue; }
    if (recente < 0 || (int32_t)(t->fingers[k].since - t->fingers[recente].since) >= 0) { recente = k; }
  }
  t->key_sob = recente >= 0 ? t->fingers[recente].firme : TECVIRT_NENHUMA;
  if (t->key_sob != antes || in) { system_request_redraw(); }
  return in;
}

int virtual_keyboard_click(virtual_keyboard_t* t, int xs, int ys)
{
  // Only the network stops the touch too quickly for the level to have seen (otherwise it would
  // double letter): the level takes care of everything it saw.
  if (!t->open || t->livre < LIVRE_AMOSTRAS || fingers_ativos(t) || tab5_now_ms() - t->key_ms < 120) { return 0; }
  return confirm(t, key_no_ponto(t->a, xs, ys, TECVIRT_NENHUMA));   // confirm() marks key_ms
}
