#include "tab5_ui.hpp"
#include "fonts/soft_fonts.hpp"
#include "tab5_glyphs.hpp"
#include "esp_memory_utils.h"   // esp_ptr_external_ram
#include <string.h>

// ---------------------------------------------------------------------------
// FONTES SUAVES - carregadas UMA vez, lidas of flash para sempre
// ---------------------------------------------------------------------------
// Cada VLWfont guarda one ponteiro para o seu DataWrapper e le o drawing of
// cada letra sob demanda (seek + read) - por isso o wrapper e o objeto of
// fonte vivem em estatico, e switch of tamanho e so switch of ponteiro (nada
// e alocado ou analisado a cada text). A tabela of metricas of cada fonte
// (~9 bytes por glifo) vai para a PSRAM no loadFont.
//
// UMA TAREFA DESENHA: o wrapper tem position of leitura propria, e duas tarefas
// escrevendo com a mesma fonte on mesmo time embaralhariam a leitura. Hoje
// so a interface desenha text (o compositor e dela) - quem one dia draw
// of outra tarefa precisa of sua propria copia.
#define FONTES_MAX 24
static FonteRapida         s_vlw[2][FONTES_MAX];   // VLW com cache of glifos (tab5_glyphs)
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

const lgfx::IFont* tab5_fonte_suave(bool negrito, int height_px)
{
  const int f = negrito ? 1 : 0;
  if (s_n[f] == 0) {   // antes of tab5_fontes_iniciar (ou sem memoria): a bitmap antiga
    return negrito ? (const lgfx::IFont*)&fonts::FreeSansBold12pt7b : (const lgfx::IFont*)&fonts::FreeSans12pt7b;
  }
  int melhor = 0;
  for (int i = 1; i < s_n[f]; ++i) {
    if (abs(s_height[f][i] - height_px) < abs(s_height[f][melhor] - height_px)) { melhor = i; }
  }
  return &s_vlw[f][melhor];
}

// De que familia e a fonte em uso: 0 body, 1 negrito, -1 outra (titulo...).
static int familia_of(const lgfx::IFont* fonte)
{
  if (fonte == &fonts::FreeSans12pt7b)     { return 0; }
  if (fonte == &fonts::FreeSansBold12pt7b) { return 1; }
  for (int f = 0; f < 2; ++f) {
    for (int i = 0; i < s_n[f]; ++i) { if (fonte == &s_vlw[f][i]) { return f; } }
  }
  return -1;
}

void tab5_text(lgfx::LovyanGFX& d, float tamanho)
{
  // Fonte que nao e of body nem negrito (one titulo ou a Font0 que alguem
  // deixou para tras): e text of CORPO que se quer aqui - none chamada
  // pede tab5_texto sobre o titulo (conferido). Voltar para o
  // body deixa o text igual independente of que foi drawn antes.
  // (A 1a versao chamava a si mesma neste caso: loop infinito in barra of
  // status, watchdog of interface a cada 5 s.)
  const int f = familia_of(d.getFont());
  d.setFont(tab5_fonte_suave(f == 1, (int)(tamanho * 8.0f + 0.5f)));
  d.setTextSize(1);
}

void tab5_fonte_body(lgfx::LovyanGFX& d)          { d.setFont(TAB5_FONTE_CORPO); d.setTextSize(1); }
void tab5_fonte_body_negrito(lgfx::LovyanGFX& d)  { d.setFont(tab5_fonte_suave(true, 29)); d.setTextSize(1); }
void tab5_titulo(lgfx::LovyanGFX& d, int height_px)   { d.setFont(tab5_fonte_suave(true, height_px)); d.setTextSize(1); }

void tab5_botao(lgfx::LovyanGFX& d, int x, int y, int l, int a,
               const char* txt, uint16_t fundo,
               uint16_t tinta, float tamanho_text)
{
  // Salva e devolve o estilo of text. Sem isto, o middle_center abaixo VAZA
  // para quem draw depois, e todo text seguinte stays centrado no x em
  // vez of comecar nele - o que empurra lines longas para fora of screen pela
  // left. A mesma pegadinha ja mordeu tab5_power_mostrar e, antes desta
  // funcao existir, quatro apps diferentes of forma independente.
  //
  // Copia POR VALOR: getTextStyle() devolve referencia para o membro interno.
  const lgfx::TextStyle estilo = d.getTextStyle();

  d.fillRoundRect(x, y, l, a, 12, fundo);
  d.setTextDatum(middle_center);
  d.setTextColor(tinta, fundo);
  // NEGRITO e DO TAMANHO DO BOTAO. O text era one
  // tamanho fixo request por quem chamava (2.0 ou 4.0), e botao alto com letra
  // of 16 px ficava "vazio" e dificil of read. Agora a letra ocupa ~55% of height of botao e encolhe so se
  // nao couber in width (com 12 px of respiro of cada lado). O
  // 'tamanho_texto' of chamador became sem efeito: quem decide e o botao, e
  // todos os botoes of sistema ficam in mesma proporcao.
  (void)tamanho_text;
  const int respiro = 12;
  int alt = (a * 55) / 100;
  for (;;) {
    d.setFont(tab5_fonte_suave(true, alt));
    d.setTextSize(1);
    if (alt <= 12 || (int)d.textWidth(txt) <= l - 2 * respiro) { break; }
    alt -= 2;
  }
  d.drawString(txt, x + l / 2, y + a / 2);
  tab5_fonte_body(d);

  d.setTextStyle(estilo);
}

void tab5_celula(lgfx::LovyanGFX& d, int cx, int y0,
                 const char* rotulo, const char* valor, uint16_t color_valor,
                 const char* subtexto)
{
  const lgfx::TextStyle estilo = d.getTextStyle();   // ver o aviso em tab5_botao()

  d.setTextDatum(top_center);

  tab5_fonte_body(d);
  tab5_text(d, 2);
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  d.drawString(rotulo, cx, y0);

  tab5_text(d, 4);
  d.setTextColor(color_valor, TFT_BLACK);
  d.drawString(valor, cx, y0 + 22);

  if (subtexto && subtexto[0]) {
    tab5_text(d, 2);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString(subtexto, cx, y0 + 66);
  }

  d.setTextStyle(estilo);
}

// O draw_gradient_wedgeline of M5GFX (LGFXBase.cpp) line por line - mesmos
// pixels, mesmo atalho of varredura (cada fileira para when exit of line,
// e a seguinte comeca onde a anterior entrou) - com UMA diferenca: a caixa
// e recortada pelo recorte em vigor, em vez of virar o recorte e ser apagada
// no end.
static float distancia_on_segmento(float px, float py, float bx, float by)
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
  if (fabsf(ax - bx) < 0.01f && fabsf(ay - by) < 0.01f) { bx += 0.01f; }   // evita divisao por zero
  const float bottom = 1.0f / 32.0f, alto = 1.0f - bottom;

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
    // 1a pass: of ys para bottom; 2a: of ys-1 para top.
    int32_t xs = x0;
    const int32_t passo = pass ? -1 : 1;
    for (int32_t yp = pass ? ys - 1 : ys; pass ? (yp >= y0) : (yp <= y1); yp += passo) {
      bool dentro = false;
      float alfa = 1.0f;
      for (int32_t xp = xs; xp <= x1; ++xp) {
        if (dentro && alfa <= bottom) { break; }
        alfa = ar - distancia_on_segmento(xp - ax, yp - ay, bax, bay);
        if (alfa <= bottom) { continue; }
        if (!dentro) { dentro = true; xs = xp; }
        if (alfa > alto) { d.drawPixel(xp, yp, color); }
        else             { d.fillRectAlpha(xp, yp, 1, 1, (uint8_t)(alfa * 255.0f), color); }
      }
    }
  }
  d.endWrite();
}

// ---------------------------------------------------------------------------
// SPRITE NA PSRAM
// ---------------------------------------------------------------------------
M5Canvas* tab5_sprite_novo(lgfx::LovyanGFX* pai, int width, int height)
{
  if (width <= 0 || height <= 0) { return nullptr; }
  M5Canvas* c = new M5Canvas(pai);
  c->setPsram(true);
  c->setColorDepth(16);
  // createSprite() pode ter caido in interna (o recuo of M5GFX when a PSRAM
  // nega): confere ONDE o buffer became, nao so se became.
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

// ---------------------------------------------------------------------------
// CORTAR ATE CABER (ver tab5_ui.hpp)
// ---------------------------------------------------------------------------
static inline bool continuacao_utf8(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

// Maior prefixo of 't' (em fronteira of caractere) que mede <= lim. 'n' e o
// tamanho of 't'; 't' inteiro NAO cabe (quem chama ja mediu).
static size_t maior_prefixo(lgfx::LovyanGFX& d, char* t, size_t n, int lim)
{
  size_t lo = 0, hi = n;   // o prefixo of tamanho lo cabe; o of tamanho hi, nao
  while (hi - lo > 1) {
    size_t middle = (lo + hi) / 2;
    while (middle > lo && continuacao_utf8(t[middle])) { --middle; }
    if (middle == lo) {      // nenhum comeco of caractere antes of middle: procura depois
      middle = (lo + hi) / 2;
      while (middle < hi && continuacao_utf8(t[middle])) { ++middle; }
      if (middle >= hi) { break; }   // one caractere so entre lo e hi, e ele nao cabe
    }
    const char guardado = t[middle];
    t[middle] = 0;
    const bool cabe = (int)d.textWidth(t) <= lim;
    t[middle] = guardado;
    if (cabe) { lo = middle; } else { hi = middle; }
  }
  return lo;
}

size_t tab5_cortar_on_caber(lgfx::LovyanGFX& d, char* t, int max_l)
{
  if (!t) { return 0; }
  const size_t n = strlen(t);
  if (n == 0 || (int)d.textWidth(t) <= max_l) { return n; }
  const size_t k = maior_prefixo(d, t, n, max_l);
  t[k] = 0;
  return k;
}

size_t tab5_cortar_com_reticencias(lgfx::LovyanGFX& d, char* t, size_t size, int max_l)
{
  if (!t || size < 4) { return t ? strlen(t) : 0; }
  const size_t n = strlen(t);
  if (n == 0 || (int)d.textWidth(t) <= max_l) { return n; }
  size_t k = maior_prefixo(d, t, n, max_l - (int)d.textWidth("..."));
  if (k + 4 > size) {   // sem espaco para os tres pontos: recua por caractere
    k = size - 4;
    while (k > 0 && continuacao_utf8(t[k])) { --k; }
  }
  memcpy(t + k, "...", 4);
  return k + 3;
}
