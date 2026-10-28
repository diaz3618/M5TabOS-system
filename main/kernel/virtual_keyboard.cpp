#include "virtual_keyboard.hpp"
#include "tab5_screen.hpp"
#include "tab5_ui.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tab5_time.hpp"   // tab5_agora_ms
#include "tab5_keyboard.hpp" // tempos da repeticao
#include <stdlib.h>
#include <string.h>

// TRES PAGINAS, mesma geometria (10/10/9/7 teclas): trocar de pagina so troca
// rotulos, nada pula de lugar. Cada tecla e UM caractere UTF-8.
//  0 letras: a fileira dos numeros fica sempre (senha, PIN e hora sem trocar
//    de pagina).
//  1 "?123": a pontuacao e os pares - parenteses, chaves, colchetes, sinais.
//  2 "=\<": o resto do ASCII e os acentos do portugues (a fonte do sistema
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
// Deitado (1280 de largura) a tecla tem ~120 px de largura e 52 de altura: o
// teclado e baixo e largo, e sobra pouca altura na tela. Em pe e o contrario:
// dez teclas em 720 px dao so ~67 px de largura, e a tela tem altura de sobra.
// Entao, em pe, a tecla cresce PARA CIMA (68 px, quase quadrada), o vao
// encolhe um pouco para devolver largura as teclas, e a letra fica maior. O
// teclado inteiro sobe ~80 px - o que a conversa em pe tem para dar.
static bool em_pe(const area_t& a) { return a.largura < a.altura; }
static int  alt_t(const area_t& a)    { return em_pe(a) ? 68 : 52; }   // altura de cada tecla
static int  vao(const area_t& a)      { return em_pe(a) ? 5 : 6; }
static int  px_letra(const area_t& a) { return em_pe(a) ? 34 : 30; }   // fonte das letras e numeros

int tecvirt_altura(const area_t& a)
{
  return vao(a) + (FILAS + 1) * (alt_t(a) + vao(a));
}

// ---------------------------------------------------------------------------
// UTF-8 E SHIFT
// ---------------------------------------------------------------------------
int tecvirt_utf8(int cp, char out[4])
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

// O codepoint da coluna 'col' de uma fileira (UTF-8). 0 = fora dela.
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

// O que a tecla de caractere digita agora (pagina e shift aplicados).
static int cp_tecla(const tecvirt_t* t, int fila, int col)
{
  if (fila < 0 || fila >= FILAS || col < 0 || col >= N_FILA[fila]) { return 0; }
  const int cp = cp_em(PAGINA[t->pagina][fila], col);
  return t->shift != TECVIRT_SHIFT_NAO ? maiuscula(cp) : cp;
}

// ---------------------------------------------------------------------------
// TECLAS ESPECIAIS
// ---------------------------------------------------------------------------
// Como no teclado do Android: shift e apagar nas pontas da fileira do "zxcv",
// e embaixo a troca de pagina, virgula, espaco, ponto, ENVIAR e fechar. Cor,
// o que faz e a largura num lugar so: o desenho completo, o de uma tecla
// isolada e o toque precisam concordar. Na fileira de comando a largura vai
// em MEIAS unidades de 1/10 da largura (20 no total); shift e apagar ocupam
// a sobra da fileira de 7 teclas (1,5 unidade cada).
//
// ICONES DESENHADOS, NAO DIGITADOS: os codigos < 0x20 viram desenho em
// pintar_tecla (shift solto/uma letra/travado e apagar).
#define ICO_SHIFT  "\x01"
#define ICO_SHIFT1 "\x02"
#define ICO_CAPS   "\x03"
#define ICO_APAGA  "\x04"
enum { E_SHIFT, E_APAGA, E_MODO, E_VIRG, E_ESPACO, E_PONTO, E_ENVIAR, E_FECHAR, NESP };
#define ID_ESP(i) (64 + (i))
#define CMD0 E_MODO   // a primeira especial da fileira de comando
typedef struct { uint16_t cor; int acao; int meias; } esp_t;
static const esp_t ESP[NESP] = {
  { TFT_LIGHTGREY,   0,              0 },   // shift / troca de pagina de simbolos (interna)
  { TFT_ORANGE,      TECVIRT_APAGAR, 0 },
  { TFT_CYAN,        0,              3 },   // ?123 / ABC (interna)
  { TFT_LIGHTGREY,   ',',            2 },
  { TFT_LIGHTGREY,   ' ',            8 },
  { TFT_LIGHTGREY,   '.',            2 },
  { TFT_GREENYELLOW, TECVIRT_ENVIAR, 3 },
  { TFT_RED,         TECVIRT_FECHAR, 2 },
};

// O rotulo da especial no estado atual.
static const char* rotulo_esp(const tecvirt_t* t, int i)
{
  switch (i) {
    case E_SHIFT:
      if (t->pagina == 1) { return "=\\<"; }
      if (t->pagina == 2) { return "?123"; }
      return t->shift == TECVIRT_SHIFT_TRAVA ? ICO_CAPS : t->shift == TECVIRT_SHIFT_UMA ? ICO_SHIFT1 : ICO_SHIFT;
    case E_APAGA:  return ICO_APAGA;
    case E_MODO:   return t->pagina ? "ABC" : "?123";
    case E_VIRG:   return ",";
    case E_ESPACO: return "espaço";
    case E_PONTO:  return ".";
    case E_ENVIAR: return "ENVIAR";
    default:       return "X";
  }
}

static uint16_t cor_esp(const tecvirt_t* t, int i)
{
  if (i == E_SHIFT && t->pagina == 0 && t->shift != TECVIRT_SHIFT_NAO) { return TFT_CYAN; }   // aceso = vale
  return ESP[i].cor;
}

// ---------------------------------------------------------------------------
// GEOMETRIA - calculada e nao tabelada: a largura muda com a rotacao.
// ---------------------------------------------------------------------------
static int topo_de(const area_t& a) { return a.y + a.altura - tecvirt_altura(a); }
static int y_fila(const area_t& a, int f) { return topo_de(a) + vao(a) + f * (alt_t(a) + vao(a)); }
static int larg_tecla(const area_t& a) { return (a.largura - 2 * vao(a) - vao(a) * 9) / 10; }   // pela maior fileira
static int recuo_fila(const area_t& a, int f)
{
  const int u = larg_tecla(a) + vao(a);
  return (a.largura - 2 * vao(a) - N_FILA[f] * u + vao(a)) / 2;
}

static bool tecla_em(const area_t& a, int fila, int col, int* x, int* y, int* l)
{
  if (fila < 0 || fila >= FILAS || col < 0 || col >= N_FILA[fila]) { return false; }
  *l = larg_tecla(a);
  *x = a.x + vao(a) + recuo_fila(a, fila) + col * (*l + vao(a));
  *y = y_fila(a, fila);
  return true;
}

static bool especial_em(const area_t& a, int i, int* x, int* y, int* l)
{
  if (i < 0 || i >= NESP) { return false; }
  const int total = a.largura - 2 * vao(a);
  if (i == E_SHIFT || i == E_APAGA) {
    const int r = recuo_fila(a, FILAS - 1);
    const int fim_letras = r + N_FILA[FILAS - 1] * (larg_tecla(a) + vao(a));
    *y = y_fila(a, FILAS - 1);
    if (i == E_SHIFT) { *x = a.x + vao(a);              *l = r - vao(a); }
    else              { *x = a.x + vao(a) + fim_letras; *l = total - fim_letras; }
    return true;
  }
  int inicio = 0;
  for (int k = CMD0; k < i; ++k) { inicio += ESP[k].meias; }
  *x = a.x + vao(a) + (inicio * total) / 20;
  *y = y_fila(a, FILAS);
  *l = ((inicio + ESP[i].meias) * total) / 20 - (inicio * total) / 20 - vao(a);
  return true;
}

static bool retangulo(const area_t& a, int id, int* x, int* y, int* l)
{
  if (id >= 64) { return especial_em(a, id - 64, x, y, l); }
  if (id < 0) { return false; }
  return tecla_em(a, id / 16, id % 16, x, y, l);
}

// O ALVO DE UMA TECLA (onde o toque vale para ela) e maior que o desenho:
//  - o vao entre teclas e de alguem: cada uma responde ate a metade dele,
//    como em teclado de celular;
//  - nas pontas das fileiras recuadas ("asdf..."), meia tecla a mais para
//    fora: o recuo era ponto morto, e o dedo que mira o "a" e cai um pouco a
//    esquerda dele quer o "a", nao nada. Shift e apagar vao ate a borda;
//  - a fileira de comando vai de borda a borda e para baixo sem limite (a
//    faixa de navegacao travada entrega o toque na ultima linha da area).
static const int ALVO_SEM_FIM = 1 << 14;

static bool alvo_de(const area_t& a, int id, int* x0, int* y0, int* x1, int* y1)
{
  const int f = (vao(a) + 1) / 2;
  int x, y, l;
  if (!retangulo(a, id, &x, &y, &l)) { return false; }
  *x0 = x - f;
  *x1 = x + l + f;
  *y0 = y - f;
  *y1 = y + alt_t(a) + f;
  if (id >= 64) {
    const int i = id - 64;
    if (i == E_SHIFT || i == CMD0) { *x0 = a.x; }
    if (i == E_APAGA || i == E_FECHAR) { *x1 = a.x + a.largura; }
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
  if (!alvo_de(a, id, &x0, &y0, &x1, &y1)) { return false; }
  return xs >= x0 - folga && xs < x1 + folga && ys >= y0 - folga && ys < y1 + folga;
}

// HISTERESE: o dedo que ja esta numa tecla so passa para a vizinha quando
// entra HISTERESE_PX nela. Na fronteira exata, o tremor do dedo (e o do
// GT911) trocava a tecla de uma amostra para outra - e a que valia era a da
// ultima amostra antes de soltar, no sorteio. 'atual' = a tecla em que o
// dedo estava (TECVIRT_NENHUMA para um toque novo).
static const int HISTERESE_PX = 8;

static int tecla_no_ponto(const area_t& a, int xs, int ys, int atual)
{
  if (atual != TECVIRT_NENHUMA && no_alvo(a, atual, xs, ys, HISTERESE_PX)) { return atual; }
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

static bool eh_apagar(int id) { return id == ID_ESP(E_APAGA); }

// ---------------------------------------------------------------------------
// DESENHO - so primitivas e a fonte do sistema (cache de glifos): nenhuma
// imagem guardada. Uma imagem do teclado pronto custaria ~420 KB de PSRAM por
// estado para economizar alguns ms numa pintura que so acontece ao abrir e ao
// trocar shift/pagina; no dia a dia so 1-2 teclas sao repintadas por toque.
// ---------------------------------------------------------------------------

// A fonte de cada especial, uma vez por largura: o MAIOR rotulo que ela pode
// ter ("ENVIAR", "espaço", "?123") desce de 2 em 2 ate caber (5 px de
// respiro de cada lado).
static void medir(tecvirt_t* t, lgfx::LovyanGFX& d)
{
  static const char* const VARIANTES[NESP][2] = {
    { "=\\<", "?123" }, { "", "" }, { "?123", "ABC" }, { ",", "" },
    { "espaço", "" }, { ".", "" }, { "ENVIAR", "" }, { "X", "" },
  };
  if (t->med_larg == t->a.largura) { return; }
  t->med_larg = t->a.largura;
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

// Seta do shift: cheia quando vale (uma letra ou travado), so o contorno
// quando solta; travado ganha a barra embaixo.
static void icone_shift(lgfx::LovyanGFX& d, int cx, int cy, char tipo)
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

// Apagar: corpo com ponta a esquerda e um X vazado na cor da tecla.
static void icone_apagar(lgfx::LovyanGFX& d, int cx, int cy, uint16_t cor)
{
  d.fillTriangle(cx - 24, cy, cx - 9, cy - 14, cx - 9, cy + 14, TFT_BLACK);
  d.fillRect(cx - 9, cy - 14, 33, 29, TFT_BLACK);
  for (int o = -1; o <= 1; ++o) {
    d.drawLine(cx + 1 + o, cy - 6, cx + 13 + o, cy + 6, cor);
    d.drawLine(cx + 13 + o, cy - 6, cx + 1 + o, cy + 6, cor);
  }
}

static void pintar_tecla(const tecvirt_t* t, lgfx::LovyanGFX& d, int id, bool acesa)
{
  int x, y, l;
  if (!retangulo(t->a, id, &x, &y, &l)) { return; }
  char letra[5];
  const char* txt;
  uint16_t cor;
  int px;
  if (id >= 64) {
    txt = rotulo_esp(t, id - 64);
    cor = cor_esp(t, id - 64);
    px  = t->px_esp[id - 64];
  } else {
    letra[tecvirt_utf8(cp_tecla(t, id / 16, id % 16), letra)] = 0;
    txt = letra;
    cor = TFT_LIGHTGREY;
    px  = px_letra(t->a);
  }
  // Acesa = AZUL, e nao "a mesma cor mais clara": debaixo do dedo so a borda
  // aparece, e uma cor DIFERENTE e o que se ve nela.
  if (acesa) { cor = TFT_SKYBLUE; }
  d.fillRoundRect(x, y, l, alt_t(t->a), 8, cor);

  const int cx = x + l / 2, cy = y + alt_t(t->a) / 2;
  if ((unsigned char)txt[0] < 0x20) {
    if (txt[0] == ICO_APAGA[0]) { icone_apagar(d, cx, cy, cor); }
    else                        { icone_shift(d, cx, cy, txt[0]); }
    return;
  }
  d.setFont(tab5_fonte_suave(true, px));
  d.setTextColor(TFT_BLACK, cor);
  d.drawString(txt, cx, cy);
}

void tecvirt_pintar(tecvirt_t* t, lgfx::LovyanGFX& d, bool completo)
{
  if (!t->aberto) { return; }
  const bool tudo = completo || t->sujo;
  const bool rot  = !tudo && t->rotulos;
  if (!tudo && !rot && t->tecla_sob == t->tecla_pinta) { return; }   // nada mudou: nem toca na tela

  const lgfx::TextStyle estilo = d.getTextStyle();
  const lgfx::IFont* fonte = d.getFont();
  medir(t, d);
  d.setTextSize(1);
  d.setTextDatum(middle_center);

  if (tudo || rot) {
    // O fundo so e limpo quando o teclado aparece ou muda de lugar; na troca
    // de shift/pagina as teclas sao opacas, os cantos ja estao pretos, e
    // repintar todas e mais simples (e quase tao barato) que escolher quais.
    if (tudo) { tab5_screen_clear(t->a.x, topo_de(t->a), t->a.largura, tecvirt_altura(t->a), TFT_BLACK); }
    for (int f = 0; f < FILAS; ++f) {
      for (int c = 0; c < N_FILA[f]; ++c) { pintar_tecla(t, d, f * 16 + c, f * 16 + c == t->tecla_sob); }
    }
    for (int i = 0; i < NESP; ++i) { pintar_tecla(t, d, ID_ESP(i), ID_ESP(i) == t->tecla_sob); }
    t->sujo = t->rotulos = false;
  } else {
    // Retorno da tecla: no maximo duas - a que o dedo deixou e a que encontrou.
    if (t->tecla_pinta != TECVIRT_NENHUMA) { pintar_tecla(t, d, t->tecla_pinta, false); }
    if (t->tecla_sob   != TECVIRT_NENHUMA) { pintar_tecla(t, d, t->tecla_sob, true); }
  }
  t->tecla_pinta = t->tecla_sob;

  d.setFont(fonte);
  d.setTextStyle(estilo);
}

// ---------------------------------------------------------------------------
// ESTADO: shift, pagina, maiuscula automatica
// ---------------------------------------------------------------------------
static void mudar(tecvirt_t* t, uint8_t shift, uint8_t pagina)
{
  if (shift == t->shift && pagina == t->pagina) { return; }
  t->shift = shift;
  t->pagina = pagina;
  t->rotulos = true;
  sistema_pedir_redesenho();
}

void tecvirt_frase(tecvirt_t* t, bool ligada) { t->frase = ligada; }

void tecvirt_contexto(tecvirt_t* t, const char* texto, int cursor)
{
  if (!t->frase || !t->aberto || t->shift == TECVIRT_SHIFT_TRAVA) { return; }
  int i = cursor;
  const int n = texto ? (int)strlen(texto) : 0;
  if (i > n) { i = n; }
  if (i < 0) { i = 0; }
  bool espaco = false;
  while (i > 0 && texto[i - 1] == ' ') { --i; espaco = true; }
  const bool inicio = i == 0 || (espaco && (texto[i - 1] == '.' || texto[i - 1] == '!' || texto[i - 1] == '?'));
  mudar(t, inicio ? TECVIRT_SHIFT_UMA : TECVIRT_SHIFT_NAO, t->pagina);
}

bool tecvirt_editar(char* buf, size_t cap, int acao, bool so_ascii)
{
  size_t n = strlen(buf);
  if (acao == TECVIRT_APAGAR) {
    while (n > 0) {
      const unsigned char b = (unsigned char)buf[--n];
      if ((b & 0xC0) != 0x80) { break; }   // continuacao de UTF-8: tira o caractere inteiro
    }
    buf[n] = 0;
    return true;
  }
  if (acao <= 0) { return false; }
  if (acao < 0x20 || (so_ascii && acao >= 0x7F)) { return true; }
  char u[4];
  const int k = tecvirt_utf8(acao, u);
  if (n + k < cap) { memcpy(buf + n, u, k); buf[n + k] = 0; }
  return true;
}

// ---------------------------------------------------------------------------
// CICLO DE VIDA
// ---------------------------------------------------------------------------
static void soltar_dedos(tecvirt_t* t)
{
  for (int k = 0; k < SISTEMA_TOQUES_MAX; ++k) {
    auto& dd = t->dedos[k];
    dd.id = -1;
    dd.tecla = dd.firme = dd.cand = TECVIRT_NENHUMA;
    dd.cand_n = 0;
    dd.eco = dd.repetiu = false;
  }
}

void tecvirt_abrir(tecvirt_t* t)
{
  if (t->aberto) { return; }
  t->aberto = true;
  t->shift = TECVIRT_SHIFT_NAO;
  t->pagina = 0;
  t->sujo = true;
  t->rotulos = false;
  t->livre = 0;
  t->tecla_sob = t->tecla_pinta = t->ultima_tecla = TECVIRT_NENHUMA;
  t->solta.tecla = TECVIRT_NENHUMA;
  soltar_dedos(t);
  sistema_travar_navegacao(true);   // o INICIO fica logo abaixo da tecla de espaco
}

void tecvirt_fechar(tecvirt_t* t)
{
  if (!t->aberto) { return; }
  t->aberto = false;
  t->tecla_sob = t->tecla_pinta = TECVIRT_NENHUMA;
  soltar_dedos(t);
  sistema_travar_navegacao(false);
}

void tecvirt_posicionar(tecvirt_t* t, const area_t& a)
{
  if (a.x != t->a.x || a.y != t->a.y || a.largura != t->a.largura || a.altura != t->a.altura) {
    t->a = a;
    t->sujo = true;
  }
}

bool tecvirt_contem(const tecvirt_t* t, int xs, int ys)
{
  return t->aberto && ys >= topo_de(t->a) && xs >= t->a.x && xs < t->a.x + t->a.largura;
}

// ---------------------------------------------------------------------------
// TOQUE
// ---------------------------------------------------------------------------

// O DEDO QUE ABRIU O TECLADO NAO DIGITA. A soltura do toque que abriu (ou
// uma amostra atrasada do GT911 logo depois dela) caia numa tecla e o texto
// ja nascia com uma letra. Ate a tela ficar sem dedo por duas amostras
// seguidas depois de abrir, nada responde (nivel e clique).
static const int LIVRE_AMOSTRAS = 2;

static bool dedos_ativos(const tecvirt_t* t)
{
  for (int k = 0; k < SISTEMA_TOQUES_MAX; ++k) { if (t->dedos[k].id >= 0) { return true; } }
  return false;
}

// Confirma uma tecla (com o freio contra repique). Shift e troca de pagina
// ficam aqui dentro; o resto vai para a lista do app. Devolve a acao, ou 0.
//
// SHIFT COMO NO CELULAR: um toque arma para UMA letra (ela sai maiuscula e o
// shift solta sozinho); outro toque em ate SHIFT_DUPLO_MS trava (caps lock);
// um toque travado solta. Nas paginas de simbolos a mesma tecla alterna
// entre as duas paginas. ESPACO numa pagina de simbolos volta as letras, e
// um acento (pagina 2) tambem: a palavra continua em letras.
static int confirmar(tecvirt_t* t, int tecla)
{
  static const uint32_t REPIQUE_MS     = 45;
  static const uint32_t SHIFT_DUPLO_MS = 400;
  if (tecla == TECVIRT_NENHUMA) { return 0; }
  const uint32_t agora = tab5_agora_ms();
  // APAGAR e a tecla batida em rajada no mesmo ponto: sem freio de repique.
  if (!eh_apagar(tecla) && tecla == t->ultima_tecla && agora - t->tecla_ms < REPIQUE_MS) { return 0; }
  t->tecla_ms = agora;
  t->ultima_tecla = (int16_t)tecla;

  if (tecla == ID_ESP(E_SHIFT)) {
    if (t->pagina) { mudar(t, t->shift, t->pagina == 1 ? 2 : 1); return 0; }
    uint8_t s = TECVIRT_SHIFT_NAO;
    if (t->shift == TECVIRT_SHIFT_NAO)                                   { s = TECVIRT_SHIFT_UMA; }
    else if (t->shift == TECVIRT_SHIFT_UMA && agora - t->shift_ms < SHIFT_DUPLO_MS) { s = TECVIRT_SHIFT_TRAVA; }
    t->shift_ms = agora;
    mudar(t, s, 0);
    return 0;
  }
  if (tecla == ID_ESP(E_MODO)) { mudar(t, t->shift, t->pagina ? 0 : 1); return 0; }

  int ac;
  if (tecla >= 64) {
    if (tecla - 64 >= NESP) { return 0; }
    ac = ESP[tecla - 64].acao;
  } else {
    ac = cp_tecla(t, tecla / 16, tecla % 16);
  }
  if (ac > 0) {
    // Depois do caractere: o shift de UMA letra solta; espaco e acento voltam as letras.
    const uint8_t s = t->shift == TECVIRT_SHIFT_UMA ? (uint8_t)TECVIRT_SHIFT_NAO : t->shift;
    const bool volta = t->pagina && (ac == ' ' || (t->pagina == 2 && eh_letra(ac)));
    mudar(t, s, volta ? 0 : t->pagina);
  }
  return ac;
}

// UM DEDO POR TECLA, CADA UM COM A SUA SOLTURA.
//
// A tecla acende sob o dedo e vale na SOLTURA daquele dedo - da para
// escorregar ate a certa antes de soltar. Digitando rapido o dedo seguinte
// desce antes de o anterior subir: cada dedo (id do controlador) tem a sua
// tecla, e as letras saem na ordem em que os dedos soltam.
//
// A TECLA QUE VALE E A FIRME, nao a da ultima amostra. Ao subir, a area de
// contato encolhe e o centro dela anda alguns pixels - a ultima amostra antes
// da soltura pegava a vizinha e a letra saia errada. O dedo que desce fixa a
// tecla na hora; escorregar para outra so troca depois de TECLA_FIRME
// amostras seguidas nela (~40 ms): a correcao de quem escorrega de proposito
// continua valendo, o tropeco da soltura nao. A tecla acesa e sempre a firme
// - o que se ve aceso e o que vai ser digitado.
//
// A LETRA SAI NA PRIMEIRA AMOSTRA SEM O DEDO. O GT911 as vezes perde um dedo
// por uma amostra, e a volta dele viraria letra dupla. A volta e reconhecida
// pelo que ela e - a mesma tecla, o mesmo ponto, NA AMOSTRA SEGUINTE a
// soltura - e marcada como ECO: acende, mas nao digita de novo. A janela e
// contada em amostras: o intervalo entre amostras varia com o desenho (20 a
// 45 ms), e um limite so em ms ficava ora curto, ora longo. ECO_MS_MAX e o
// teto: numa volta lenta (desenho pesado), "a amostra seguinte" pode chegar
// tarde o bastante para ser uma batida nova de verdade no mesmo ponto.
// Escorregou para outra tecla, deixa de ser eco.
//
// O ECO NAO VALE PARA O APAGAR, e nunca encadeia. Batido rapido no mesmo
// ponto, o apagar subia e descia dentro da janela do eco: a batida era
// engolida, e a soltura dela virava a referencia da seguinte - uma rajada
// inteira sumia. So a soltura que DIGITOU serve de referencia, e o apagar
// nunca e eco.
//
// APAGAR SEGURADO REPETE, como em qualquer teclado: depois de REP_INICIO_MS
// com o dedo parado nele, apaga um a cada REP_MS.
int tecvirt_nivel(tecvirt_t* t, int* acoes, int max)
{
  static const int      SALTO_PX      = 45;
  static const uint32_t ECO_AMOSTRAS  = 1;
  static const uint32_t ECO_MS_MAX    = 60;   // e nunca mais que isto (volta lenta)
  static const uint8_t  TECLA_FIRME   = 2;
  // Os mesmos tempos do teclado fisico (hal/tab5_keyboard.hpp).
  static const uint32_t REP_INICIO_MS = TAB5_KEY_REPEAT_START_MS;
  static const uint32_t REP_MS        = TAB5_KEY_REPEAT_MS;
  if (!t->aberto) { return 0; }
  ++t->amostra;
  sistema_toque_t q[SISTEMA_TOQUES_MAX];
  const int n = sistema_toques(q, SISTEMA_TOQUES_MAX);
  const area_t ap = sistema_area();
  // Dedo na faixa do teclado: o toque e dele, a soltura nao vira clique em
  // nada (ver sistema_consumir_toque). Inclusive na carencia de abertura.
  for (int i = 0; i < n; ++i) {
    if (tecvirt_contem(t, ap.x + q[i].x, ap.y + q[i].y)) { sistema_consumir_toque(); break; }
  }
  if (t->livre < LIVRE_AMOSTRAS) {
    if (n) { t->livre = 0; } else { ++t->livre; }
    soltar_dedos(t);
    return 0;
  }

  auto tecla_de = [&](const sistema_toque_t& p, int atual) {
    return (int16_t)tecla_no_ponto(t->a, ap.x + p.x, ap.y + p.y, atual);
  };
  int na = 0;
  auto anotar = [&](int ac) { if (ac && na < max) { acoes[na++] = ac; } };
  const int16_t antes = t->tecla_sob;
  const uint32_t agora = tab5_agora_ms();

  // 1) Dedos que ja estavam: seguem a tecla sob eles, ou soltam.
  for (int k = 0; k < SISTEMA_TOQUES_MAX; ++k) {
    auto& dd = t->dedos[k];
    if (dd.id < 0) { continue; }
    int achou = -1;
    for (int i = 0; i < n; ++i) { if (q[i].id == dd.id) { achou = i; break; } }
    if (achou >= 0) {
      const int16_t sob = tecla_de(q[achou], dd.tecla);
      const int dx = q[achou].x - dd.x, dy = q[achou].y - dd.y;
      if (sob != dd.tecla && (abs(dx) > SALTO_PX || abs(dy) > SALTO_PX)) {
        // O CONTROLADOR REAPROVEITA O ID: o dedo sobe e o proximo desce
        // dentro de uma amostra, e para o sistema e o "mesmo dedo" que pulou
        // de tecla. Um dedo de verdade nao anda mais que ~45 px numa amostra;
        // um salto maior e um toque novo - a tecla anterior vale e a nova
        // comeca firme.
        if (!dd.eco && !dd.repetiu) { anotar(confirmar(t, dd.firme)); }
        dd.firme = sob;
        dd.cand = TECVIRT_NENHUMA; dd.cand_n = 0;
        dd.desde = dd.rep_ms = agora;
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
          dd.rep_ms = agora;
        }
      }
      dd.tecla = sob;
      dd.x = q[achou].x; dd.y = q[achou].y;
      // Repeticao do APAGAR segurado.
      if (eh_apagar(dd.firme) && agora - dd.rep_ms >= (dd.repetiu ? REP_MS : REP_INICIO_MS)) {
        anotar(confirmar(t, dd.firme));
        dd.repetiu = true;
        dd.rep_ms = agora;
      }
    } else {
      const bool digita = !dd.eco && !dd.repetiu;
      if (digita) { anotar(confirmar(t, dd.firme)); }
      // So a soltura que digitou e referencia de eco - um eco nunca gera outro.
      t->solta = { digita ? dd.firme : (int16_t)TECVIRT_NENHUMA, dd.x, dd.y, t->amostra, agora };
      dd.id = -1;
      dd.tecla = dd.firme = dd.cand = TECVIRT_NENHUMA;
      dd.cand_n = 0;
      dd.eco = dd.repetiu = false;
    }
  }
  // 2) Dedos novos: a tecla sob o ponto de descida ja nasce firme.
  for (int i = 0; i < n; ++i) {
    bool conhecido = false;
    for (int k = 0; k < SISTEMA_TOQUES_MAX; ++k) { if (t->dedos[k].id == q[i].id) { conhecido = true; break; } }
    if (conhecido) { continue; }
    for (int k = 0; k < SISTEMA_TOQUES_MAX; ++k) {
      auto& dd = t->dedos[k];
      if (dd.id >= 0) { continue; }
      dd.id = (int8_t)q[i].id;
      dd.tecla = dd.firme = tecla_de(q[i], TECVIRT_NENHUMA);
      dd.cand = TECVIRT_NENHUMA; dd.cand_n = 0;
      dd.x = q[i].x; dd.y = q[i].y;
      dd.desde = dd.rep_ms = agora;
      dd.repetiu = false;
      dd.eco = dd.firme != TECVIRT_NENHUMA && dd.firme == t->solta.tecla
            && t->amostra - t->solta.amostra <= ECO_AMOSTRAS && agora - t->solta.ms <= ECO_MS_MAX
            && !eh_apagar(dd.firme)
            && abs(dd.x - t->solta.x) <= SALTO_PX && abs(dd.y - t->solta.y) <= SALTO_PX;
      break;
    }
  }
  // 3) A tecla acesa e a firme do dedo que desceu por ultimo.
  int recente = -1;
  for (int k = 0; k < SISTEMA_TOQUES_MAX; ++k) {
    if (t->dedos[k].id < 0) { continue; }
    if (recente < 0 || (int32_t)(t->dedos[k].desde - t->dedos[recente].desde) >= 0) { recente = k; }
  }
  t->tecla_sob = recente >= 0 ? t->dedos[recente].firme : TECVIRT_NENHUMA;
  if (t->tecla_sob != antes || na) { sistema_pedir_redesenho(); }
  return na;
}

int tecvirt_clique(tecvirt_t* t, int xs, int ys)
{
  // So a rede para o toque rapido demais para o nivel ter visto (senao daria
  // letra dupla): o nivel cuida de tudo que ele viu.
  if (!t->aberto || t->livre < LIVRE_AMOSTRAS || dedos_ativos(t) || tab5_agora_ms() - t->tecla_ms < 120) { return 0; }
  return confirmar(t, tecla_no_ponto(t->a, xs, ys, TECVIRT_NENHUMA));   // confirmar() marca tecla_ms
}
