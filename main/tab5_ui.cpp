#include "tab5_ui.hpp"
#include "fonts/soft_fonts.hpp"
#include "tab5_glyphs.hpp"
#include "esp_memory_utils.h"   // esp_ptr_external_ram
#include <string.h>

// ---------------------------------------------------------------------------
// FONTES SUAVES - carregadas UMA vez, lidas da flash para sempre
// ---------------------------------------------------------------------------
// Cada VLWfont guarda um ponteiro para o seu DataWrapper e le o desenho de
// cada letra sob demanda (seek + read) - por isso o wrapper e o objeto da
// fonte vivem em estatico, e trocar de tamanho e so trocar de ponteiro (nada
// e alocado ou analisado a cada texto). A tabela de metricas de cada fonte
// (~9 bytes por glifo) vai para a PSRAM no loadFont.
//
// UMA TAREFA DESENHA: o wrapper tem posicao de leitura propria, e duas tarefas
// escrevendo com a mesma fonte ao mesmo tempo embaralhariam a leitura. Hoje
// so a interface desenha texto (o compositor e dela) - quem um dia desenhar
// de outra tarefa precisa da sua propria copia.
#define FONTES_MAX 24
static FonteRapida         s_vlw[2][FONTES_MAX];   // VLW com cache de glifos (tab5_glyphs)
static lgfx::PointerWrapper s_dados[2][FONTES_MAX];
static int                 s_altura[2][FONTES_MAX];
static int                 s_n[2] = { 0, 0 };

void tab5_fontes_iniciar(void)
{
  const fonte_suave_t* tab[2] = { FONTES_CORPO, FONTES_NEGRITO };
  const int            n[2]   = { FONTES_CORPO_N, FONTES_NEGRITO_N };
  for (int f = 0; f < 2; ++f) {
    s_n[f] = 0;
    for (int i = 0; i < n[f] && s_n[f] < FONTES_MAX; ++i) {
      const int k = s_n[f];
      s_dados[f][k].set(tab[f][i].dados, tab[f][i].tamanho);
      if (s_vlw[f][k].loadFont(&s_dados[f][k])) {
        s_altura[f][k] = tab[f][i].altura;
        ++s_n[f];
      }
    }
  }
}

const lgfx::IFont* tab5_fonte_suave(bool negrito, int altura_px)
{
  const int f = negrito ? 1 : 0;
  if (s_n[f] == 0) {   // antes do tab5_fontes_iniciar (ou sem memoria): a bitmap antiga
    return negrito ? (const lgfx::IFont*)&fonts::FreeSansBold12pt7b : (const lgfx::IFont*)&fonts::FreeSans12pt7b;
  }
  int melhor = 0;
  for (int i = 1; i < s_n[f]; ++i) {
    if (abs(s_altura[f][i] - altura_px) < abs(s_altura[f][melhor] - altura_px)) { melhor = i; }
  }
  return &s_vlw[f][melhor];
}

// De que familia e a fonte em uso: 0 corpo, 1 negrito, -1 outra (titulo...).
static int familia_de(const lgfx::IFont* fonte)
{
  if (fonte == &fonts::FreeSans12pt7b)     { return 0; }
  if (fonte == &fonts::FreeSansBold12pt7b) { return 1; }
  for (int f = 0; f < 2; ++f) {
    for (int i = 0; i < s_n[f]; ++i) { if (fonte == &s_vlw[f][i]) { return f; } }
  }
  return -1;
}

void tab5_texto(lgfx::LovyanGFX& d, float tamanho)
{
  // Fonte que nao e de corpo nem negrito (um titulo ou a Font0 que alguem
  // deixou para tras): e texto de CORPO que se quer aqui - nenhuma chamada
  // pede tab5_texto sobre o titulo (conferido). Voltar para o
  // corpo deixa o texto igual independente do que foi desenhado antes.
  // (A 1a versao chamava a si mesma neste caso: loop infinito na barra de
  // status, watchdog da interface a cada 5 s.)
  const int f = familia_de(d.getFont());
  d.setFont(tab5_fonte_suave(f == 1, (int)(tamanho * 8.0f + 0.5f)));
  d.setTextSize(1);
}

void tab5_fonte_corpo(lgfx::LovyanGFX& d)          { d.setFont(TAB5_FONTE_CORPO); d.setTextSize(1); }
void tab5_fonte_corpo_negrito(lgfx::LovyanGFX& d)  { d.setFont(tab5_fonte_suave(true, 29)); d.setTextSize(1); }
void tab5_titulo(lgfx::LovyanGFX& d, int altura_px)   { d.setFont(tab5_fonte_suave(true, altura_px)); d.setTextSize(1); }

void tab5_botao(lgfx::LovyanGFX& d, int x, int y, int l, int a,
               const char* txt, uint16_t fundo,
               uint16_t tinta, float tamanho_texto)
{
  // Salva e devolve o estilo de texto. Sem isto, o middle_center abaixo VAZA
  // para quem desenhar depois, e todo texto seguinte fica centrado no x em
  // vez de comecar nele - o que empurra linhas longas para fora da tela pela
  // esquerda. A mesma pegadinha ja mordeu tab5_power_mostrar e, antes desta
  // funcao existir, quatro apps diferentes de forma independente.
  //
  // Copia POR VALOR: getTextStyle() devolve referencia para o membro interno.
  const lgfx::TextStyle estilo = d.getTextStyle();

  d.fillRoundRect(x, y, l, a, 12, fundo);
  d.setTextDatum(middle_center);
  d.setTextColor(tinta, fundo);
  // NEGRITO e DO TAMANHO DO BOTAO. O texto era um
  // tamanho fixo pedido por quem chamava (2.0 ou 4.0), e botao alto com letra
  // de 16 px ficava "vazio" e dificil de ler. Agora a letra ocupa ~55% da altura do botao e encolhe so se
  // nao couber na largura (com 12 px de respiro de cada lado). O
  // 'tamanho_texto' do chamador ficou sem efeito: quem decide e o botao, e
  // todos os botoes do sistema ficam na mesma proporcao.
  (void)tamanho_texto;
  const int respiro = 12;
  int alt = (a * 55) / 100;
  for (;;) {
    d.setFont(tab5_fonte_suave(true, alt));
    d.setTextSize(1);
    if (alt <= 12 || (int)d.textWidth(txt) <= l - 2 * respiro) { break; }
    alt -= 2;
  }
  d.drawString(txt, x + l / 2, y + a / 2);
  tab5_fonte_corpo(d);

  d.setTextStyle(estilo);
}

void tab5_celula(lgfx::LovyanGFX& d, int cx, int y0,
                 const char* rotulo, const char* valor, uint16_t cor_valor,
                 const char* subtexto)
{
  const lgfx::TextStyle estilo = d.getTextStyle();   // ver o aviso em tab5_botao()

  d.setTextDatum(top_center);

  tab5_fonte_corpo(d);
  tab5_texto(d, 2);
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  d.drawString(rotulo, cx, y0);

  tab5_texto(d, 4);
  d.setTextColor(cor_valor, TFT_BLACK);
  d.drawString(valor, cx, y0 + 22);

  if (subtexto && subtexto[0]) {
    tab5_texto(d, 2);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString(subtexto, cx, y0 + 66);
  }

  d.setTextStyle(estilo);
}

// O draw_gradient_wedgeline do M5GFX (LGFXBase.cpp) linha por linha - mesmos
// pixels, mesmo atalho de varredura (cada fileira para quando sai da linha,
// e a seguinte comeca onde a anterior entrou) - com UMA diferenca: a caixa
// e recortada pelo recorte em vigor, em vez de virar o recorte e ser apagada
// no fim.
static float distancia_ao_segmento(float px, float py, float bx, float by)
{
  const float d = (px * bx + py * by) / (bx * bx + by * by);
  const float h = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
  const float dx = px - bx * h, dy = py - by * h;
  return sqrtf(dx * dx + dy * dy);
}

void tab5_linha_grossa(lgfx::LovyanGFX& d, float ax, float ay, float bx, float by,
                       float r, uint16_t cor)
{
  if (r < 0.0f) { return; }
  if (fabsf(ax - bx) < 0.01f && fabsf(ay - by) < 0.01f) { bx += 0.01f; }   // evita divisao por zero
  const float baixo = 1.0f / 32.0f, alto = 1.0f - baixo;

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
    // 1a passada: de ys para baixo; 2a: de ys-1 para cima.
    int32_t xs = x0;
    const int32_t passo = pass ? -1 : 1;
    for (int32_t yp = pass ? ys - 1 : ys; pass ? (yp >= y0) : (yp <= y1); yp += passo) {
      bool dentro = false;
      float alfa = 1.0f;
      for (int32_t xp = xs; xp <= x1; ++xp) {
        if (dentro && alfa <= baixo) { break; }
        alfa = ar - distancia_ao_segmento(xp - ax, yp - ay, bax, bay);
        if (alfa <= baixo) { continue; }
        if (!dentro) { dentro = true; xs = xp; }
        if (alfa > alto) { d.drawPixel(xp, yp, cor); }
        else             { d.fillRectAlpha(xp, yp, 1, 1, (uint8_t)(alfa * 255.0f), cor); }
      }
    }
  }
  d.endWrite();
}

// ---------------------------------------------------------------------------
// SPRITE NA PSRAM
// ---------------------------------------------------------------------------
M5Canvas* tab5_sprite_novo(lgfx::LovyanGFX* pai, int largura, int altura)
{
  if (largura <= 0 || altura <= 0) { return nullptr; }
  M5Canvas* c = new M5Canvas(pai);
  c->setPsram(true);
  c->setColorDepth(16);
  // createSprite() pode ter caido na interna (o recuo do M5GFX quando a PSRAM
  // nega): confere ONDE o buffer ficou, nao so se ficou.
  if (!c->createSprite(largura, altura) || !esp_ptr_external_ram(c->getBuffer())) {
    c->deleteSprite();
    delete c;
    return nullptr;
  }
  c->setFont(TAB5_FONTE_CORPO);
  return c;
}

void tab5_sprite_soltar(M5Canvas** sprite)
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

// Maior prefixo de 't' (em fronteira de caractere) que mede <= lim. 'n' e o
// tamanho de 't'; 't' inteiro NAO cabe (quem chama ja mediu).
static size_t maior_prefixo(lgfx::LovyanGFX& d, char* t, size_t n, int lim)
{
  size_t lo = 0, hi = n;   // o prefixo de tamanho lo cabe; o de tamanho hi, nao
  while (hi - lo > 1) {
    size_t meio = (lo + hi) / 2;
    while (meio > lo && continuacao_utf8(t[meio])) { --meio; }
    if (meio == lo) {      // nenhum comeco de caractere antes do meio: procura depois
      meio = (lo + hi) / 2;
      while (meio < hi && continuacao_utf8(t[meio])) { ++meio; }
      if (meio >= hi) { break; }   // um caractere so entre lo e hi, e ele nao cabe
    }
    const char guardado = t[meio];
    t[meio] = 0;
    const bool cabe = (int)d.textWidth(t) <= lim;
    t[meio] = guardado;
    if (cabe) { lo = meio; } else { hi = meio; }
  }
  return lo;
}

size_t tab5_cortar_ao_caber(lgfx::LovyanGFX& d, char* t, int max_l)
{
  if (!t) { return 0; }
  const size_t n = strlen(t);
  if (n == 0 || (int)d.textWidth(t) <= max_l) { return n; }
  const size_t k = maior_prefixo(d, t, n, max_l);
  t[k] = 0;
  return k;
}

size_t tab5_cortar_com_reticencias(lgfx::LovyanGFX& d, char* t, size_t tam, int max_l)
{
  if (!t || tam < 4) { return t ? strlen(t) : 0; }
  const size_t n = strlen(t);
  if (n == 0 || (int)d.textWidth(t) <= max_l) { return n; }
  size_t k = maior_prefixo(d, t, n, max_l - (int)d.textWidth("..."));
  if (k + 4 > tam) {   // sem espaco para os tres pontos: recua por caractere
    k = tam - 4;
    while (k > 0 && continuacao_utf8(t[k])) { --k; }
  }
  memcpy(t + k, "...", 4);
  return k + 3;
}
