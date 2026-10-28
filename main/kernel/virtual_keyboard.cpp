#include "virtual_keyboard.hpp"
#include "tab5_screen.hpp"
#include "tab5_ui.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tab5_time.hpp"   // tab5_now_ms
#include "tab5_keyboard.hpp" // tempos of repeticao
#include <stdlib.h>
#include <string.h>

// TRES PAGINAS, mesma geometria (10/10/9/7 teclas): switch of pagina so troca
// rotulos, nada pula of lugar. Cada key e UM caractere UTF-8.
//  0 letras: a fileira dos numeros stays sempre (senha, PIN e hora sem switch
//    of pagina).
//  1 "?123": a pontuacao e os pares - parenteses, chaves, colchetes, sinais.
//  2 "=\<": o resto of ASCII e os acentos of portugues (a fonte of sistema
//    tem estes e nao muito mais - ver tools/generate_fonts.py). Os
//    acentos seguem o shift, como as letras.
static const int N_FILA[TECVIRT_FILAS] = { 10, 10, 9, 7 };
static const char* const PAGINA[3][TECVIRT_FILAS] = {
  { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" },
  { "1234567890", "@#$%&-+()/", "*\"':;!?_=", "[]{}<>\\" },
  { "1234567890", "áàâãéêíóôõ", "úç°ºª|~^`", "•…«»×÷±" },
};

#define FILAS   TECVIRT_FILAS

// ---------------------------------------------------------------------------
// MEDIDAS POR ORIENTACAO
// ---------------------------------------------------------------------------
// Deitado (1280 of width) a key tem ~120 px of width e 52 of height: o
// keyboard e bottom e largo, e sobra pouca height in screen. Em pe e o contrario:
// dez teclas em 720 px dao so ~67 px of width, e a screen tem height of sobra.
// Entao, em pe, a key cresce PARA CIMA (68 px, quase quadrada), o vao
// encolhe one pouco para return width as teclas, e a letra stays maior. O
// keyboard inteiro sobe ~80 px - o que a conversa em pe tem para dar.
static bool em_pe(const area_t& a) { return a.width < a.height; }
static int  alt_t(const area_t& a)    { return em_pe(a) ? 68 : 52; }   // height of cada key
static int  vao(const area_t& a)      { return em_pe(a) ? 5 : 6; }
static int  px_letra(const area_t& a) { return em_pe(a) ? 34 : 30; }   // fonte das letras e numeros

int virtual_keyboard_height(const area_t& a)
{
  return vao(a) + (FILAS + 1) * (alt_t(a) + vao(a));
}

// ---------------------------------------------------------------------------
// UTF-8 E SHIFT
// ---------------------------------------------------------------------------
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

// O codepoint of coluna 'col' of uma fileira (UTF-8). 0 = fora dela.
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

// Letra (ASCII ou Latin-1 minuscula/maiuscula)? E a maiuscula dela.
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

// O que a key of caractere digita now (pagina e shift aplicados).
static int cp_key(const virtual_keyboard_t* t, int fila, int col)
{
  if (fila < 0 || fila >= FILAS || col < 0 || col >= N_FILA[fila]) { return 0; }
  const int cp = cp_em(PAGINA[t->pagina][fila], col);
  return t->shift != TECVIRT_SHIFT_NAO ? maiuscula(cp) : cp;
}

// ---------------------------------------------------------------------------
// TECLAS ESPECIAIS
// ---------------------------------------------------------------------------
// Como no keyboard of Android: shift e erase nas pontas of fileira of "zxcv",
// e embaixo a troca of pagina, virgula, espaco, ponto, ENVIAR e close. Cor,
// o que faz e a width num lugar so: o drawing complete, o of uma key
// isolada e o touch precisam concordar. Na fileira of comando a width vai
// em MEIAS unidades of 1/10 of width (20 no total); shift e erase ocupam
// a sobra of fileira of 7 teclas (1,5 unidade cada).
//
// ICONES DESENHADOS, NAO DIGITADOS: os codigos < 0x20 viram drawing em
// pintar_tecla (shift released/uma letra/travado e erase).
#define ICO_SHIFT  "\x01"
#define ICO_SHIFT1 "\x02"
#define ICO_CAPS   "\x03"
#define ICO_APAGA  "\x04"
enum { E_SHIFT, E_APAGA, E_MODO, E_VIRG, E_ESPACO, E_PONTO, E_ENVIAR, E_FECHAR, NESP };
#define ID_ESP(i) (64 + (i))
#define CMD0 E_MODO   // a primeira especial of fileira of comando
typedef struct { uint16_t color; int acao; int meias; } esp_t;
static const esp_t ESP[NESP] = {
  { TFT_LIGHTGREY,   0,              0 },   // shift / troca of pagina of simbolos (interna)
  { TFT_ORANGE,      TECVIRT_APAGAR, 0 },
  { TFT_CYAN,        0,              3 },   // ?123 / ABC (interna)
  { TFT_LIGHTGREY,   ',',            2 },
  { TFT_LIGHTGREY,   ' ',            8 },
  { TFT_LIGHTGREY,   '.',            2 },
  { TFT_GREENYELLOW, TECVIRT_ENVIAR, 3 },
  { TFT_RED,         TECVIRT_FECHAR, 2 },
};

// O rotulo of especial no estado current.
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
  if (i == E_SHIFT && t->pagina == 0 && t->shift != TECVIRT_SHIFT_NAO) { return TFT_CYAN; }   // aceso = vale
  return ESP[i].color;
}

// ---------------------------------------------------------------------------
// GEOMETRIA - calculada e nao tabelada: a width muda com a rotation.
// ---------------------------------------------------------------------------
static int topo_of(const area_t& a) { return a.y + a.height - virtual_keyboard_height(a); }
static int y_fila(const area_t& a, int f) { return topo_of(a) + vao(a) + f * (alt_t(a) + vao(a)); }
static int larg_key(const area_t& a) { return (a.width - 2 * vao(a) - vao(a) * 9) / 10; }   // pela maior fileira
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

// O ALVO DE UMA TECLA (onde o touch vale para ela) e maior que o drawing:
//  - o vao entre teclas e of alguem: cada uma responde ate a metade dele,
//    como em keyboard of celular;
//  - nas pontas das fileiras recuadas ("asdf..."), meia key a mais para
//    fora: o recuo era ponto morto, e o finger que mira o "a" e cai one pouco a
//    left dele quer o "a", nao nada. Shift e erase vao ate a borda;
//  - a fileira of comando vai of borda a borda e para bottom sem limit (a
//    faixa of navigation travada entrega o touch in ultima line of area).
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

// HISTERESE: o finger que ja esta numa key so passa para a vizinha when
// entra HISTERESE_PX nela. Na fronteira exata, o tremor of finger (e o of
// GT911) trocava a key of uma sample para outra - e a que valia era a of
// ultima sample antes of release, no sorteio. 'current' = a key em que o
// finger estava (TECVIRT_NENHUMA para one touch novo).
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

// ---------------------------------------------------------------------------
// DESENHO - so primitivas e a fonte of sistema (cache of glifos): none
// imagem guardada. Uma imagem of keyboard pronto custaria ~420 KB of PSRAM por
// estado para economizar alguns ms numa paint que so acontece on open e on
// switch shift/pagina; no dia a dia so 1-2 teclas sao repintadas por touch.
// ---------------------------------------------------------------------------

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

// Seta of shift: cheia when vale (uma letra ou travado), so o contorno
// when released; travado ganha a barra embaixo.
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

// Apagar: body com ponta a left e one X vazado in color of key.
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
  // Acesa = AZUL, e nao "a mesma color mais clara": debaixo of finger so a borda
  // aparece, e uma color DIFERENTE e o que se ve nela.
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
  if (!everything && !rot && t->key_sob == t->key_pinta) { return; }   // nada changed: nem toca in screen

  const lgfx::TextStyle estilo = d.getTextStyle();
  const lgfx::IFont* fonte = d.getFont();
  medir(t, d);
  d.setTextSize(1);
  d.setTextDatum(middle_center);

  if (everything || rot) {
    // O fundo so e limpo when o keyboard aparece ou muda of lugar; in troca
    // of shift/pagina as teclas sao opacas, os cantos ja estao pretos, e
    // repintar todas e mais simples (e quase tao barato) que escolher quais.
    if (everything) { tab5_screen_clear(t->a.x, topo_of(t->a), t->a.width, virtual_keyboard_height(t->a), TFT_BLACK); }
    for (int f = 0; f < FILAS; ++f) {
      for (int c = 0; c < N_FILA[f]; ++c) { paint_key(t, d, f * 16 + c, f * 16 + c == t->key_sob); }
    }
    for (int i = 0; i < NESP; ++i) { paint_key(t, d, ID_ESP(i), ID_ESP(i) == t->key_sob); }
    t->dirty = t->rotulos = false;
  } else {
    // Retorno of key: no maximum duas - a que o finger deixou e a que encontrou.
    if (t->key_pinta != TECVIRT_NENHUMA) { paint_key(t, d, t->key_pinta, false); }
    if (t->key_sob   != TECVIRT_NENHUMA) { paint_key(t, d, t->key_sob, true); }
  }
  t->key_pinta = t->key_sob;

  d.setFont(fonte);
  d.setTextStyle(estilo);
}

// ---------------------------------------------------------------------------
// ESTADO: shift, pagina, maiuscula automatica
// ---------------------------------------------------------------------------
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
      if ((b & 0xC0) != 0x80) { break; }   // continuacao of UTF-8: tira o caractere inteiro
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

// ---------------------------------------------------------------------------
// CICLO DE VIDA
// ---------------------------------------------------------------------------
static void release_fingers(virtual_keyboard_t* t)
{
  for (int k = 0; k < SISTEMA_TOQUES_MAX; ++k) {
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
  system_lock_navigation(true);   // o INICIO stays logo abaixo of key of espaco
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

// ---------------------------------------------------------------------------
// TOQUE
// ---------------------------------------------------------------------------

// O DEDO QUE ABRIU O TECLADO NAO DIGITA. A soltura of touch que abriu (ou
// uma sample atrasada of GT911 logo depois dela) caia numa key e o text
// ja nascia com uma letra. Ate a screen ficar sem finger por duas amostras
// seguidas depois of open, nada responde (level e click).
static const int LIVRE_AMOSTRAS = 2;

static bool fingers_ativos(const virtual_keyboard_t* t)
{
  for (int k = 0; k < SISTEMA_TOQUES_MAX; ++k) { if (t->fingers[k].id >= 0) { return true; } }
  return false;
}

// Confirma uma key (com o freio contra repique). Shift e troca of pagina
// ficam aqui dentro; o resto vai para a lista of app. Devolve a acao, ou 0.
//
// SHIFT COMO NO CELULAR: one touch arma para UMA letra (ela exit maiuscula e o
// shift released sozinho); outro touch em ate SHIFT_DUPLO_MS trava (caps lock);
// one touch travado released. Nas paginas of simbolos a mesma key alterna
// entre as duas paginas. ESPACO numa pagina of simbolos iteration as letras, e
// one acento (pagina 2) tambem: a palavra continua em letras.
static int confirm(virtual_keyboard_t* t, int key)
{
  static const uint32_t REPIQUE_MS     = 45;
  static const uint32_t SHIFT_DUPLO_MS = 400;
  if (key == TECVIRT_NENHUMA) { return 0; }
  const uint32_t now = tab5_now_ms();
  // APAGAR e a key batida em rajada no mesmo ponto: sem freio of repique.
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
    // Depois of caractere: o shift of UMA letra released; espaco e acento voltam as letras.
    const uint8_t s = t->shift == TECVIRT_SHIFT_UMA ? (uint8_t)TECVIRT_SHIFT_NAO : t->shift;
    const bool iteration = t->pagina && (ac == ' ' || (t->pagina == 2 && eh_letra(ac)));
    mudar(t, s, iteration ? 0 : t->pagina);
  }
  return ac;
}

// UM DEDO POR TECLA, CADA UM COM A SUA SOLTURA.
//
// A key acende sob o finger e vale in SOLTURA daquele finger - of para
// escorregar ate a certa antes of release. Digitando rapido o finger seguinte
// desce antes of o anterior raise: cada finger (id of controlador) tem a sua
// key, e as letras saem in order em que os fingers soltam.
//
// A TECLA QUE VALE E A FIRME, nao a of ultima sample. Ao raise, a area of
// contato encolhe e o centro dela anda alguns pixels - a ultima sample antes
// of soltura pegava a vizinha e a letra saia errada. O finger que desce fixa a
// key in hora; escorregar para outra so troca depois of TECLA_FIRME
// amostras seguidas nela (~40 ms): a correcao of quem escorrega of proposito
// continua valendo, o tropeco of soltura nao. A key acesa e sempre a firme
// - o que se ve aceso e o que vai ser digitado.
//
// A LETRA SAI NA PRIMEIRA AMOSTRA SEM O DEDO. O GT911 as vezes perde one finger
// por uma sample, e a iteration dele viraria letra dupla. A iteration e reconhecida
// pelo que ela e - a mesma key, o mesmo ponto, NA AMOSTRA SEGUINTE a
// soltura - e marcada como ECO: acende, mas nao digita of novo. A window e
// contada em amostras: o intervalo entre amostras varia com o drawing (20 a
// 45 ms), e one limit so em ms ficava ora curto, ora longo. ECO_MS_MAX e o
// teto: numa iteration lenta (drawing pesado), "a sample seguinte" pode chegar
// tarde o bastante para ser uma batida nova of verdade no mesmo ponto.
// Escorregou para outra key, deixa of ser eco.
//
// O ECO NAO VALE PARA O APAGAR, e nunca encadeia. Batido rapido no mesmo
// ponto, o erase subia e descia dentro of window of eco: a batida era
// engolida, e a soltura dela virava a referencia of seguinte - uma rajada
// inteira sumia. So a soltura que DIGITOU serve of referencia, e o erase
// nunca e eco.
//
// APAGAR SEGURADO REPETE, como em qualquer keyboard: depois of REP_INICIO_MS
// com o finger parado nele, apaga one a cada REP_MS.
int virtual_keyboard_level(virtual_keyboard_t* t, int* actions, int max)
{
  static const int      SALTO_PX      = 45;
  static const uint32_t ECO_AMOSTRAS  = 1;
  static const uint32_t ECO_MS_MAX    = 60;   // e nunca mais que isto (iteration lenta)
  static const uint8_t  TECLA_FIRME   = 2;
  // Os mesmos tempos of keyboard fisico (hal/tab5_keyboard.hpp).
  static const uint32_t REP_INICIO_MS = TAB5_KEY_REPEAT_START_MS;
  static const uint32_t REP_MS        = TAB5_KEY_REPEAT_MS;
  if (!t->open) { return 0; }
  ++t->sample;
  system_touch_t q[SISTEMA_TOQUES_MAX];
  const int n = system_touches(q, SISTEMA_TOQUES_MAX);
  const area_t ap = system_area();
  // Dedo in faixa of keyboard: o touch e dele, a soltura nao vira click em
  // nada (ver sistema_consumir_toque). Inclusive in carencia of abertura.
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

  // 1) Dedos que ja estavam: seguem a key sob eles, ou soltam.
  for (int k = 0; k < SISTEMA_TOQUES_MAX; ++k) {
    auto& dd = t->fingers[k];
    if (dd.id < 0) { continue; }
    int achou = -1;
    for (int i = 0; i < n; ++i) { if (q[i].id == dd.id) { achou = i; break; } }
    if (achou >= 0) {
      const int16_t sob = key_of(q[achou], dd.key);
      const int dx = q[achou].x - dd.x, dy = q[achou].y - dd.y;
      if (sob != dd.key && (abs(dx) > SALTO_PX || abs(dy) > SALTO_PX)) {
        // O CONTROLADOR REAPROVEITA O ID: o finger sobe e o next desce
        // dentro of uma sample, e para o sistema e o "mesmo finger" que pulou
        // of key. Um finger of verdade nao anda mais que ~45 px numa sample;
        // one salto maior e one touch novo - a key anterior vale e a nova
        // comeca firme.
        if (!dd.eco && !dd.repetiu) { anotar(confirm(t, dd.firme)); }
        dd.firme = sob;
        dd.cand = TECVIRT_NENHUMA; dd.cand_n = 0;
        dd.since = dd.rep_ms = now;
        dd.eco = dd.repetiu = false;
      } else if (sob == dd.firme) {
        dd.cand = TECVIRT_NENHUMA; dd.cand_n = 0;      // voltou: o desvio nao conta
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
      // Repeticao of APAGAR segurado.
      if (eh_erase(dd.firme) && now - dd.rep_ms >= (dd.repetiu ? REP_MS : REP_INICIO_MS)) {
        anotar(confirm(t, dd.firme));
        dd.repetiu = true;
        dd.rep_ms = now;
      }
    } else {
      const bool digita = !dd.eco && !dd.repetiu;
      if (digita) { anotar(confirm(t, dd.firme)); }
      // So a soltura que digitou e referencia of eco - one eco nunca gera outro.
      t->released = { digita ? dd.firme : (int16_t)TECVIRT_NENHUMA, dd.x, dd.y, t->sample, now };
      dd.id = -1;
      dd.key = dd.firme = dd.cand = TECVIRT_NENHUMA;
      dd.cand_n = 0;
      dd.eco = dd.repetiu = false;
    }
  }
  // 2) Dedos novos: a key sob o ponto of press_start ja nasce firme.
  for (int i = 0; i < n; ++i) {
    bool conhecido = false;
    for (int k = 0; k < SISTEMA_TOQUES_MAX; ++k) { if (t->fingers[k].id == q[i].id) { conhecido = true; break; } }
    if (conhecido) { continue; }
    for (int k = 0; k < SISTEMA_TOQUES_MAX; ++k) {
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
  // 3) A key acesa e a firme of finger que desceu por ultimo.
  int recente = -1;
  for (int k = 0; k < SISTEMA_TOQUES_MAX; ++k) {
    if (t->fingers[k].id < 0) { continue; }
    if (recente < 0 || (int32_t)(t->fingers[k].since - t->fingers[recente].since) >= 0) { recente = k; }
  }
  t->key_sob = recente >= 0 ? t->fingers[recente].firme : TECVIRT_NENHUMA;
  if (t->key_sob != antes || in) { system_request_redraw(); }
  return in;
}

int virtual_keyboard_click(virtual_keyboard_t* t, int xs, int ys)
{
  // So a rede para o touch rapido demais para o level ter visto (senao daria
  // letra dupla): o level cuida of everything que ele viu.
  if (!t->open || t->livre < LIVRE_AMOSTRAS || fingers_ativos(t) || tab5_now_ms() - t->key_ms < 120) { return 0; }
  return confirm(t, key_no_ponto(t->a, xs, ys, TECVIRT_NENHUMA));   // confirm() marca tecla_ms
}
