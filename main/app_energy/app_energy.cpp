// APP ENERGIA - o estado da bateria agora e dois graficos no molde dos
// celulares (a tela de bateria do Android e do iOS):
//
//   1. A GRANDEZA ESCOLHIDA ao longo do tempo (por padrao o nivel da bateria),
//      com area preenchida, eixo com numeros redondos e as horas de CARGA
//      tingidas de verde atras da curva: carga e descarga se leem pela forma,
//      sem precisar ler numero. Periodo 30 MIN (anel da RAM) ou 24 H.
//   2. GASTO POR HORA (mAh) nas ultimas 24 h, em barras: tela acesa em cima,
//      repouso embaixo, e uma faixa verde sob as horas em que houve carga.
//
// Nas 24 h os dois graficos dividem o MESMO eixo de tempo (uma coluna por
// hora do relogio, a ultima e a hora corrente): a barra fica embaixo do
// trecho da curva a que ela se refere.
//
// Em cima: a porcentagem grande com a estimativa ("dura ~X" / "cheia em ~X")
// e um INDICE de seis cartoes com os valores do momento. Tocar num cartao
// escolhe a grandeza do grafico 1. Tocar no grafico 1 poe um cursor com a
// hora e o valor; tocar numa barra mostra o detalhe daquela hora.
//
// Dados das 24 h: os arquivos de ontem e de hoje (services/energy_log, uma
// amostra por minuto, com a hora real), lidos pelo WORKER, completados com o
// anel de 30 min da RAM - o que o registro ainda nao gravou (lotes de 5 min).
//
// Redesenho por REGIAO: topo (heroi + cartoes) a cada segundo; os dois
// graficos, com os titulos, so quando chega dado novo ou muda uma selecao.
// Nenhum toque pede passada completa: cada regiao limpa o proprio retangulo.

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
  lgfx::color565( 90, 170, 255),   // bateria: azul, para nao se confundir com o verde da carga
  TFT_ORANGE,
  TFT_GREENYELLOW,
  lgfx::color565(190, 150, 255),
  lgfx::color565(255,  90,  80),
};

#define COR_CARGA        lgfx::color565( 60, 200,  90)   // carga: o mesmo verde em todo o app
#define COR_CARGA_FUNDO  lgfx::color565( 14,  50,  26)   // tinta das horas de carga atras da curva
#define COR_GASTO_TELA   TFT_ORANGE
#define COR_GASTO_REP    lgfx::color565(130,  75,  20)
#define COR_CARTAO       lgfx::color565( 26,  26,  34)
#define COR_CARTAO_SEL   lgfx::color565( 42,  42,  56)
#define COR_EIXO         lgfx::color565(110, 110, 124)

static serie_t s_serie   = SERIE_BATERIA;
static bool    s_24h     = true;      // grafico 1: true = 24 H (cartao + RAM); false = 30 MIN (RAM)
static int     s_cursor  = -1;        // x do cursor no grafico 1 (relativo a area); -1 = sem
static int     s_hora_sel = -1;       // barra escolhida no grafico 2 (0..23); -1 = totais
static bool    s_graf_sujo = true;    // um toque mudou o que os graficos mostram
static pintura_t s_p;

// ---------------------------------------------------------------------------
// 24 H: um slot por minuto da janela, carregado pelo worker
// ---------------------------------------------------------------------------
// A janela comeca na hora cheia de 23 h atras: 24 colunas de hora inteira, a
// ultima sendo a hora corrente (parcial). Alinhar na hora do relogio e o que
// deixa as barras do grafico 2 com rotulos "14h", "15h" em vez de fatias de
// 60 min que comecam num minuto qualquer.
#define MIN_DIA 1440
typedef struct {
  uint32_t t0;                 // epoch UTC do slot 0 (0 = sem relogio)
  float    v[MIN_DIA], ma[MIN_DIA], temp[MIN_DIA];
  int8_t   pct[MIN_DIA];       // a % que o sistema mostrava (estimador), -1 = sem
  uint8_t  tem[MIN_DIA];       // 0 = sem amostra, 1 = acordado, 2 = em repouso
} dia_t;

// Uma fonte de pontos: n pontos, x de 0..1, os valores, e o instante 't'
// (24 H: o slot, minuto desde t0; 30 MIN: segundos antes de agora).
typedef struct {
  int     n;
  float   x[MIN_DIA], v[MIN_DIA], ma[MIN_DIA], temp[MIN_DIA];
  int8_t  pct[MIN_DIA];
  uint8_t rep[MIN_DIA];        // 2 = em repouso; outro = tela acesa (ou nao se sabe)
  bool    quebra[MIN_DIA];     // buraco antes deste ponto: aparelho desligado
  int32_t t[MIN_DIA];
} pontos_t;

// Um vertice por coluna de pixel do grafico 1 (ver montar_vertices).
#define VERT_MAX 720
typedef struct {
  int     n;
  int16_t px[VERT_MAX];
  float   val[VERT_MAX];
  bool    carga[VERT_MAX], quebra[VERT_MAX];
  int32_t t[VERT_MAX];
} vertices_t;

typedef struct { float tela, repouso, carga; int min_tela; bool tem; } hora_t;

// Tudo o que so a interface usa, num bloco na PSRAM alocado ao abrir.
typedef struct {
  pontos_t     p24, p30;
  vertices_t   vt;
  tab5_point_t ram[TAB5_HIST_N];
  hora_t       horas[24];
  float        tot_gasto, tot_carga, tot_wh;
  int          tot_min_tela;
} memoria_t;
static memoria_t* s_m = nullptr;

static dia_t*        s_dia = nullptr;      // o que a tela mostra
static dia_t*        s_carga = nullptr;    // onde o worker escreve
static char          s_carga_nome[2][40];  // ontem e hoje (podem ser o mesmo)
static uint32_t      s_carga_t0 = 0;
static volatile bool s_carga_pronta = false;
static bool          s_carga_pedida = false;
static uint32_t      s_gravadas_lidas = UINT32_MAX;   // registro ja refletido nas 24 h
static uint32_t      s_dia_ver = 0;                    // sobe a cada carga nova

// Inicio da janela de 24 h para o instante atual; 0 sem relogio acertado.
static uint32_t janela_t0(void)
{
  const uint32_t agora = tab5_time_epoch();
  if (!agora) { return 0; }
  struct tm lt;
  tab5_local_time(agora, &lt);
  return agora - (uint32_t)(lt.tm_min * 60 + lt.tm_sec) - 23u * 3600u;
}

static void ler_arquivo(dia_t* d, const char* nome)
{
  FILE* f = tab5_sd_open(TAB5_SD_POWER, nome, "r");
  if (!f) { return; }
  char linha[160];
  while (fgets(linha, sizeof(linha), f)) {
    unsigned long ep; char hora[8]; float v, ma, mw, temp; int pct, mhz, rep;
    if (sscanf(linha, "ENE3,%lu,%7[^,],%f,%f,%f,%d,%d,%d,%f", &ep, hora, &v, &ma, &mw, &pct, &mhz, &rep, &temp) < 9) { continue; }
    if (ep < d->t0) { continue; }
    const uint32_t m = ((uint32_t)ep - d->t0) / 60;
    if (m >= MIN_DIA) { continue; }
    d->tem[m] = rep ? 2 : 1;
    d->v[m] = v; d->ma[m] = ma; d->temp[m] = temp;
    d->pct[m] = (int8_t)((pct >= 0 && pct <= 100) ? pct : -1);
  }
  fclose(f);
}

static void trabalho_24h(void*)
{
  dia_t* d = s_carga;
  memset(d, 0, sizeof(*d));
  d->t0 = s_carga_t0;
  if (d->t0 && tab5_sd_mount()) {
    ler_arquivo(d, s_carga_nome[0]);
    if (strcmp(s_carga_nome[0], s_carga_nome[1]) != 0) { ler_arquivo(d, s_carga_nome[1]); }
    tab5_sd_unmount();
  }
  s_carga_pronta = true;
}

static void pedir_24h(void)
{
  if (s_carga_pedida || !s_carga) { return; }
  s_carga_t0 = janela_t0();
  s_carga_nome[0][0] = s_carga_nome[1][0] = 0;
  if (s_carga_t0) {
    tab5_day_filename(s_carga_t0, "energia-", ".csv", s_carga_nome[0], sizeof(s_carga_nome[0]));
    tab5_day_filename((uint32_t)time(nullptr), "energia-", ".csv", s_carga_nome[1], sizeof(s_carga_nome[1]));
  }
  s_carga_pronta = false;
  s_carga_pedida = sistema_trabalho(trabalho_24h, nullptr);
}

static void ao_aviso(uint32_t av)
{
  if ((av & AVISO_TRABALHO) && s_carga_pronta) {
    s_carga_pronta = false;
    s_carga_pedida = false;
    dia_t* t = s_dia; s_dia = s_carga; s_carga = t;
    ++s_dia_ver;
    sistema_pedir_redesenho();
  }
}

static bool ao_abrir(void)
{
  if (!s_dia)   { s_dia   = (dia_t*)heap_caps_calloc(1, sizeof(dia_t), MALLOC_CAP_SPIRAM); }
  if (!s_carga) { s_carga = (dia_t*)heap_caps_calloc(1, sizeof(dia_t), MALLOC_CAP_SPIRAM); }
  if (!s_m)     { s_m     = (memoria_t*)heap_caps_calloc(1, sizeof(memoria_t), MALLOC_CAP_SPIRAM); }
  if (!s_dia || !s_carga || !s_m) { return false; }
  s_carga_pedida = false;
  s_carga_pronta = false;
  s_gravadas_lidas = UINT32_MAX;   // as 24 h sao lidas ao abrir
  s_cursor = -1;
  s_hora_sel = -1;
  s_graf_sujo = true;
  pintura_pedir(&s_p);
  return true;
}

static void ao_fechar(void)
{
  // (O sistema ja esperou o worker antes de chamar isto.)
  heap_caps_free(s_dia);   s_dia = nullptr;
  heap_caps_free(s_carga); s_carga = nullptr;
  heap_caps_free(s_m);     s_m = nullptr;
}

// ---------------------------------------------------------------------------
// GEOMETRIA (relativa a area, 720 x 1144 em retrato; termina em ~1120) - a mesma para desenho e toque
// ---------------------------------------------------------------------------
static const int M = 16;                               // margem lateral
static const int HERO_Y = 8, HERO_A = 76;              // % grande + estado
static const int CART_Y = HERO_Y + HERO_A + 8;         // indice: 3 x 2 cartoes
static const int CART_A = 92, CART_V = 12;             // altura e vao entre cartoes
static const int TOPO_A = CART_Y + 2 * CART_A + CART_V;
static const int EIXO_L = 64;                          // coluna dos numeros do eixo Y

static const int G1_Y = TOPO_A + 20;                   // titulo + 30 MIN | 24 H
static const int SEG_A = 48, SEG_L = 240;
static const int G1_PLOT_Y = G1_Y + SEG_A + 16, G1_A = 320;
static const int G1_FIM = G1_PLOT_Y + G1_A + 34;       // + rotulos de tempo

static const int G2_Y = G1_FIM + 22;                   // titulo + legenda
static const int G2_PLOT_Y = G2_Y + 48, G2_A = 216;
static const int G2_FAIXA = 8;                         // faixa verde de carga sob as barras
static const int G2_DET_Y = G2_PLOT_Y + G2_A + G2_FAIXA + 40;   // duas linhas de detalhe

static int cart_l(int larg) { return (larg - 2 * M - 2 * CART_V) / 3; }
static int seg_x(int larg)  { return larg - M - SEG_L; }
static int plot_x(void)     { return M + EIXO_L; }
static int plot_l(int larg) { return larg - 2 * M - EIXO_L; }

static const float TAM_BOTAO = 3.625f;     // = corpo 29 px nativo

// Fontes suaves do sistema, nas alturas nativas (sem escala).
static void fc(lgfx::LovyanGFX& d, int px) { d.setFont(tab5_fonte_suave(false, px)); d.setTextSize(1); }
static void fn(lgfx::LovyanGFX& d, int px) { d.setFont(tab5_fonte_suave(true, px));  d.setTextSize(1); }

// Cor escurecida (f de 0..256) - o preenchimento da area sob a curva.
static uint16_t escurecer(uint16_t c, int f)
{
  const int r = ((c >> 11) & 31) * f / 256, g = ((c >> 5) & 63) * f / 256, b = (c & 31) * f / 256;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t cor_nivel(int pct)
{
  return pct < 0 ? (uint16_t)TFT_DARKGREY : pct < 20 ? (uint16_t)TFT_RED
       : pct < 50 ? (uint16_t)TFT_YELLOW : (uint16_t)COR_CARGA;
}

static void duracao(char* out, size_t tam, float horas)
{
  const int min = (int)(horas * 60.0f + 0.5f);
  if (min < 60) { snprintf(out, tam, "%d min", min); }
  else          { snprintf(out, tam, "%d h %02d min", min / 60, min % 60); }
}

// ---------------------------------------------------------------------------
// SERIES
// ---------------------------------------------------------------------------
// A serie BATERIA e a MESMA % que o sistema mostra (o estimador, gravada a
// cada amostra) - nao a curva de tensao recalculada aqui, que diria outra
// coisa logo depois de uma carga e poria o grafico em desacordo com a barra.
static float valor(const pontos_t* p, serie_t s, int i)
{
  switch (s) {
    case SERIE_TENSAO:   return p->v[i];
    case SERIE_CORRENTE: return p->ma[i];                                       // COM sinal
    case SERIE_POTENCIA: return p->v[i] * fabsf(p->ma[i]) / 1000.0f;            // W
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
static float faixa_minima(serie_t s)
{
  return (s == SERIE_TENSAO) ? 0.10f : (s == SERIE_CORRENTE) ? 40.0f
       : (s == SERIE_POTENCIA) ? 0.2f : 4.0f;
}

// ESCALA COM NUMEROS REDONDOS. O grafico antigo punha so o minimo e o maximo
// medidos (ex.: -183 e -97 mA) no canto: nenhuma linha da grade tinha valor,
// e a curva nao se lia. Aqui os limites sao multiplos de um passo 1/2/2,5/5,
// e cada linha da grade tem o seu numero no eixo.
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

static void numero_eixo(char* out, size_t tam, float v, float passo)
{
  if (fabsf(v) < passo * 1e-3f) { v = 0; }   // nada de "-0"
  snprintf(out, tam, passo >= 1.0f ? "%.0f" : passo >= 0.1f ? "%.1f" : "%.2f", (double)v);
}

// ---------------------------------------------------------------------------
// MONTAGEM DOS PONTOS
// ---------------------------------------------------------------------------
static void montar_30(pontos_t* p)
{
  const int n = tab5_power_history(s_m->ram);
  for (int i = 0; i < n; ++i) {
    // Alinhado a direita: "agora" sempre na borda, enchendo nos primeiros 30 min.
    p->x[i] = (float)(TAB5_HIST_N - n + i) / (TAB5_HIST_N - 1);
    p->v[i] = s_m->ram[i].volts; p->ma[i] = s_m->ram[i].mA; p->temp[i] = s_m->ram[i].temp; p->pct[i] = (int8_t)s_m->ram[i].pct;
    p->rep[i] = 0;
    p->quebra[i] = (i == 0);
    p->t[i] = (n - 1 - i) * TAB5_HIST_SEG;
  }
  p->n = n;
}

static void montar_24h(pontos_t* p)
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
    p->quebra[k] = (m - ultimo) > 3;   // buraco de mais de 3 min: aparelho desligado
    p->t[k] = m;
    ultimo = m;
  }
  // O QUE O CARTAO AINDA NAO TEM: o registro grava em lotes de 5 min, entao
  // completa com o anel da RAM, do minuto seguinte ao ultimo gravado ate
  // agora. O anel nao guarda o repouso; esses poucos minutos contam como
  // tela acesa (o app esta aberto, afinal).
  const uint32_t agora = tab5_time_epoch();
  if (!agora) { return; }
  const int n = tab5_power_history(s_m->ram);
  for (int i = 0; i < n; ++i) {
    const uint32_t ep = (uint32_t)agora - (uint32_t)((n - 1 - i) * TAB5_HIST_SEG);
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

// Cada ponto das 24 h vale um minuto: mAh = mA / 60.
static void montar_horas(const pontos_t* p)
{
  memset(s_m->horas, 0, sizeof(s_m->horas));
  s_m->tot_gasto = s_m->tot_carga = s_m->tot_wh = 0;
  s_m->tot_min_tela = 0;
  for (int i = 0; i < p->n; ++i) {
    hora_t* h = &s_m->horas[p->t[i] / 60];
    h->tem = true;
    const bool tela = (p->rep[i] != 2);
    if (tela) { ++h->min_tela; ++s_m->tot_min_tela; }
    if (p->ma[i] < 0) {
      const float mah = -p->ma[i] / 60.0f;
      if (tela) { h->tela += mah; } else { h->repouso += mah; }
      s_m->tot_gasto += mah;
      s_m->tot_wh += p->v[i] * mah / 1000.0f;
    } else {
      h->carga += p->ma[i] / 60.0f;
      s_m->tot_carga += p->ma[i] / 60.0f;
    }
  }
}

// UM VERTICE POR COLUNA DE PIXEL. Nas 24 h sao 1440 minutos em ~600 px:
// varios pontos caem na mesma coluna, e a media deles vira o vertice. Isso
// suaviza o ruido minuto a minuto da corrente (que virava um borrao de
// zigue-zague) sem filtro a parte, e o custo do desenho passa a depender da
// largura, nao do numero de pontos.
static void montar_vertices(const pontos_t* p, serie_t s, int gx, int gl)
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

// ---------------------------------------------------------------------------
// DESENHO - TOPO (heroi + indice), a cada segundo
// ---------------------------------------------------------------------------
// Bateria desenhada: contorno, enchimento pela %, raio por cima na carga.
static void icone_bateria(lgfx::LovyanGFX& d, int x, int y, int l, int a, int pct, bool carregando)
{
  const uint16_t cor = cor_nivel(pct);
  d.fillRoundRect(x, y, l, a, 7, TFT_LIGHTGREY);
  d.fillRoundRect(x + 3, y + 3, l - 6, a - 6, 5, TFT_BLACK);
  d.fillRoundRect(x + l + 2, y + a / 2 - 7, 5, 14, 2, TFT_LIGHTGREY);
  const int cheio = pct < 0 ? 0 : (l - 12) * (pct > 100 ? 100 : pct) / 100;
  if (cheio > 0) { d.fillRoundRect(x + 6, y + 6, cheio, a - 12, 3, cor); }
  if (carregando) {
    const int cx = x + l / 2, cy = y + a / 2;
    d.fillTriangle(cx + 4, cy - 14, cx - 9, cy + 2, cx + 1, cy + 2, TFT_WHITE);
    d.fillTriangle(cx - 4, cy + 14, cx + 9, cy - 2, cx - 1, cy - 2, TFT_WHITE);
  }
}

static void desenhar_topo(lgfx::LovyanGFX& d, const area_t& a)
{
  const tab5_measurement_t m = tab5_power_latest();
  tab5_screen_clear(a.x, a.y, a.largura, TOPO_A, TFT_BLACK);
  char buf[64], dur[24];

  // HEROI. O estado vem da corrente do instante (a mesma regra de tres
  // estados da barra de status); a ESTIMATIVA vem da media dos ultimos
  // 30 min - a corrente de um segundo pula demais para virar previsao.
  const int n = tab5_power_history(s_m->ram);
  float media = m.mA;
  if (n >= 4) { media = 0; for (int i = 0; i < n; ++i) { media += s_m->ram[i].mA; } media /= n; }
  const bool carregando = m.mA > 20.0f, na_bateria = m.mA < -20.0f;

  const int y = a.y + HERO_Y;
  fn(d, 56); d.setTextDatum(top_left);
  d.setTextColor(cor_nivel(m.pct), TFT_BLACK);
  if (m.pct < 0) { snprintf(buf, sizeof(buf), "--"); } else { snprintf(buf, sizeof(buf), "%d%%", m.pct); }
  d.drawString(buf, a.x + M, y);
  const int fim_pct = a.x + M + d.textWidth(buf);
  icone_bateria(d, fim_pct + 18, y + (d.fontHeight() - 34) / 2, 68, 34, m.pct, carregando);
  // Tocar no heroi escolhe a serie BATERIA: a faixa a esquerda diz que esta escolhida.
  if (s_serie == SERIE_BATERIA) { d.fillRoundRect(a.x + 4, y + 8, 5, HERO_A - 24, 2, SERIE_COR[SERIE_BATERIA]); }

  const char* estado; uint16_t cor_estado; buf[0] = 0;
  if (carregando) {
    estado = "Carregando"; cor_estado = COR_CARGA;
    if (media > 20.0f && m.pct >= 0 && m.pct < 100) {
      duracao(dur, sizeof(dur), (100 - m.pct) / 100.0f * TAB5_CAPACITY_MAH / media);
      snprintf(buf, sizeof(buf), "cheia em ~%s", dur);
    }
  } else if (na_bateria) {
    estado = "Na bateria"; cor_estado = TFT_WHITE;
    if (media < -20.0f && m.pct >= 0) {
      const float h = m.pct / 100.0f * TAB5_CAPACITY_MAH / -media;
      if (h < 100.0f) { duracao(dur, sizeof(dur), h); snprintf(buf, sizeof(buf), "dura ~%s neste ritmo", dur); }
    }
  } else {
    estado = "Na tomada"; cor_estado = COR_CARGA;
    snprintf(buf, sizeof(buf), "carga completa");
  }
  const int xr = a.x + a.largura - M;
  fn(d, 32); d.setTextDatum(top_right); d.setTextColor(cor_estado, TFT_BLACK);
  d.drawString(estado, xr, y + 2);
  fc(d, 24); d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString(buf, xr, y + 42);

  // INDICE: seis cartoes. Os cinco primeiros sao as series - tocar escolhe o
  // grafico 1; o sexto e o gasto das 24 h (o total do grafico 2).
  float temp = -273.0f; const bool tem_temp = tab5_sensors_temp(&temp);
  const int cl = cart_l(a.largura);
  for (int i = 0; i < 6; ++i) {
    const int cx = a.x + M + (i % 3) * (cl + CART_V), cy = a.y + CART_Y + (i / 3) * (CART_A + CART_V);
    const bool sel = (i < N_SERIES && (int)s_serie == i);
    const uint16_t fundo = sel ? COR_CARTAO_SEL : COR_CARTAO;
    d.fillRoundRect(cx, cy, cl, CART_A, 12, fundo);
    if (sel) {
      d.drawRoundRect(cx, cy, cl, CART_A, 12, SERIE_COR[i]);
      d.drawRoundRect(cx + 1, cy + 1, cl - 2, CART_A - 2, 11, SERIE_COR[i]);
    }
    uint16_t cor = (i < N_SERIES) ? SERIE_COR[i] : (uint16_t)TFT_WHITE;
    switch (i) {
      case SERIE_BATERIA:  if (m.pct < 0) { snprintf(buf, sizeof(buf), "--"); } else { snprintf(buf, sizeof(buf), "%d %%", m.pct); } break;
      case SERIE_CORRENTE: snprintf(buf, sizeof(buf), "%+.0f mA", (double)m.mA); cor = m.mA > 20.0f ? (uint16_t)COR_CARGA : cor; break;
      case SERIE_POTENCIA: snprintf(buf, sizeof(buf), "%.2f W", (double)(m.mW / 1000.0f)); break;
      case SERIE_TENSAO:   snprintf(buf, sizeof(buf), "%.2f V", (double)m.volts); break;
      case SERIE_TEMP:     if (tem_temp) { snprintf(buf, sizeof(buf), "%.1f °C", (double)temp); } else { snprintf(buf, sizeof(buf), "--"); } break;
      default:             if (s_dia && s_dia->t0) { snprintf(buf, sizeof(buf), "%.0f mAh", (double)s_m->tot_gasto); } else { snprintf(buf, sizeof(buf), "--"); } break;
    }
    fc(d, 21); d.setTextDatum(top_left);
    d.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, fundo);
    d.drawString(i < N_SERIES ? SERIE_CARTAO[i] : "Gasto 24 h", cx + 14, cy + 10);
    fn(d, 36); d.setTextColor(cor, fundo);
    d.drawString(buf, cx + 14, cy + 40);
  }
  d.setTextDatum(top_left);
}

// ---------------------------------------------------------------------------
// DESENHO - EIXO DE TEMPO (o mesmo para os dois graficos nas 24 h)
// ---------------------------------------------------------------------------
static void eixo_tempo(lgfx::LovyanGFX& d, int gx, int gl, int gy, int ga, bool h24, bool grade)
{
  const int divs = h24 ? 4 : 2;     // 24 H: a cada 6 h; 30 MIN: a cada 15 min
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

static void aviso_no_grafico(lgfx::LovyanGFX& d, int gx, int gl, int gy, int ga, const char* txt)
{
  fc(d, 24); d.setTextColor(TFT_DARKGREY, TFT_BLACK); d.setTextDatum(middle_center);
  d.drawString(txt, gx + gl / 2, gy + ga / 2);
  d.setTextDatum(top_left);
}

static void desenhar_eixo_y(lgfx::LovyanGFX& d, int gx, int gl, int gy, int ga, float lo, float hi, float passo, bool so_meio_cheio)
{
  char buf[16];
  fc(d, 21); d.setTextColor(TFT_LIGHTGREY, TFT_BLACK); d.setTextDatum(middle_right);
  const int n = (int)((hi - lo) / passo + 0.5f);
  for (int k = 0; k <= n; ++k) {
    const float v = lo + k * passo;
    const int y = gy + ga - 1 - (int)((v - lo) * (ga - 1) / (hi - lo) + 0.5f);
    d.drawFastHLine(gx, y, gl, k == 0 ? COR_EIXO : TAB5_GRAFICO_COR_GRADE);
    if (so_meio_cheio && (k % 2)) { continue; }   // bateria: rotula 0/50/100, grade a cada 25
    numero_eixo(buf, sizeof(buf), v, passo);
    d.drawString(buf, gx - 10, y);
  }
  d.setTextDatum(top_left);
}

// ---------------------------------------------------------------------------
// DESENHO - GRAFICO 1: a grandeza escolhida ao longo do tempo
// ---------------------------------------------------------------------------
static void desenhar_grafico1(lgfx::LovyanGFX& d, const area_t& a)
{
  const int gx = a.x + plot_x(), gl = plot_l(a.largura), gy = a.y + G1_PLOT_Y, ga = G1_A;
  tab5_screen_clear(a.x, a.y + G1_Y, a.largura, G1_FIM - G1_Y, TFT_BLACK);
  char buf[48];

  // Cabecalho: o que e o grafico (com unidade) e o periodo.
  fn(d, 28); d.setTextDatum(middle_left); d.setTextColor(SERIE_COR[s_serie], TFT_BLACK);
  d.drawString(SERIE_TITULO[s_serie], a.x + M, a.y + G1_Y + SEG_A / 2);
  const int sx = a.x + seg_x(a.largura), sy = a.y + G1_Y, meio = SEG_L / 2;
  tab5_botao(d, sx,        sy, meio, SEG_A, "30 MIN", s_24h ? (uint16_t)TFT_BLACK : (uint16_t)TFT_WHITE,
             s_24h ? (uint16_t)TFT_LIGHTGREY : (uint16_t)TFT_BLACK, TAM_BOTAO);
  tab5_botao(d, sx + meio, sy, meio, SEG_A, "24 H",   s_24h ? (uint16_t)TFT_WHITE : (uint16_t)TFT_BLACK,
             s_24h ? (uint16_t)TFT_BLACK : (uint16_t)TFT_LIGHTGREY, TAM_BOTAO);
  d.drawRoundRect(sx, sy, SEG_L, SEG_A, 10, TFT_LIGHTGREY);
  d.setTextDatum(top_left);

  if (s_24h && (!s_dia || !s_dia->t0)) {
    d.drawFastHLine(gx, gy + ga - 1, gl, COR_EIXO);
    aviso_no_grafico(d, gx, gl, gy, ga, s_carga_pedida ? "reading the card..." : "24 H needs the clock (GPS, mesh, or NTP)");
    return;
  }
  const pontos_t* p = s_24h ? &s_m->p24 : &s_m->p30;
  eixo_tempo(d, gx, gl, gy, ga, s_24h, false);
  montar_vertices(p, s_serie, gx, gl);
  vertices_t* v = &s_m->vt;
  if (v->n < 2) {
    d.drawFastHLine(gx, gy + ga - 1, gl, COR_EIXO);
    aviso_no_grafico(d, gx, gl, gy, ga, s_24h ? "ainda sem pontos (1 por minuto)" : "juntando dados (1 ponto a cada 30 s)...");
    return;
  }

  float lo, hi, passo;
  if (s_serie == SERIE_BATERIA) { lo = 0; hi = 100; passo = 25; }
  else {
    float mn = v->val[0], mx = v->val[0];
    for (int i = 1; i < v->n; ++i) { if (v->val[i] < mn) { mn = v->val[i]; } if (v->val[i] > mx) { mx = v->val[i]; } }
    // Corrente: o zero entra na escala - a linha do zero separa carga de descarga.
    if (s_serie == SERIE_CORRENTE) { if (mn > 0) { mn = 0; } if (mx < 0) { mx = 0; } }
    escala(mn, mx, faixa_minima(s_serie), &lo, &hi, &passo);
  }
  #define YV(val) (gy + ga - 1 - (int)((((val) < lo ? lo : (val) > hi ? hi : (val)) - lo) * (ga - 1) / (hi - lo) + 0.5f))

  // 1) As horas de CARGA tingidas atras de tudo (na corrente o proprio sinal
  //    ja diz isso, com o preenchimento verde acima do zero).
  if (s_serie != SERIE_CORRENTE) {
    for (int i = 0; i < v->n; ++i) {
      if (!v->carga[i]) { continue; }
      const bool liga = (i + 1 < v->n && !v->quebra[i + 1]);
      const int x1 = liga ? v->px[i + 1] : v->px[i] + 2;
      d.fillRect(v->px[i], gy, x1 - v->px[i] + 1, ga, COR_CARGA_FUNDO);
    }
  }
  // 2) Grade com numeros e a vertical do tempo.
  desenhar_eixo_y(d, gx, gl, gy, ga, lo, hi, passo, s_serie == SERIE_BATERIA);
  eixo_tempo(d, gx, gl, gy, ga, s_24h, true);

  // 3) AREA sob a curva, coluna a coluna, interpolando entre os vertices.
  //    Corrente: preenche ate o ZERO - verde acima (carga), laranja abaixo.
  const uint16_t cor = SERIE_COR[s_serie];
  const int y_base = (s_serie == SERIE_CORRENTE) ? YV(0.0f) : gy + ga - 2;
  const uint16_t fill = escurecer(cor, 72);
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

  // 4) A LINHA por cima (~3 px, borda suavizada). Ponto isolado vira bolinha.
  for (int i = 0; i < v->n; ++i) {
    const int y = YV(v->val[i]);
    if (i > 0 && !v->quebra[i]) { tab5_linha_grossa(d, v->px[i - 1], YV(v->val[i - 1]), v->px[i], y, 1.5f, cor); }
    else if (i + 1 >= v->n || v->quebra[i + 1]) { d.fillCircle(v->px[i], y, 2, cor); }
  }

  // 5) CURSOR: o vertice mais perto do toque, com a hora e o valor.
  if (s_cursor >= 0) {
    const int alvo = a.x + s_cursor;
    int k = 0;
    for (int i = 1; i < v->n; ++i) { if (abs(v->px[i] - alvo) < abs(v->px[k] - alvo)) { k = i; } }
    const int cx = v->px[k], cy = YV(v->val[k]);
    d.drawFastVLine(cx, gy, ga, TFT_WHITE);
    d.fillCircle(cx, cy, 7, TFT_WHITE);
    d.fillCircle(cx, cy, 4, cor);
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
    d.drawRoundRect(bx, gy + 6, bl, ba, 8, cor);
    d.setTextDatum(middle_left); d.setTextColor(TFT_WHITE, COR_CARTAO_SEL);
    d.drawString(buf, bx + 12, gy + 6 + ba / 2);
    d.setTextDatum(top_left);
  }
  #undef YV
}

// ---------------------------------------------------------------------------
// DESENHO - GRAFICO 2: gasto por hora nas 24 h
// ---------------------------------------------------------------------------
static void quadradinho(lgfx::LovyanGFX& d, int* xr, int ym, uint16_t cor, const char* txt)
{
  // Legenda escrita da direita para a esquerda: devolve em *xr onde a proxima termina.
  const int w = d.textWidth(txt);
  d.setTextDatum(middle_right); d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString(txt, *xr, ym);
  d.fillRoundRect(*xr - w - 22, ym - 8, 16, 16, 3, cor);
  *xr -= w + 22 + 20;
}

static void desenhar_grafico2(lgfx::LovyanGFX& d, const area_t& a)
{
  const int gx = a.x + plot_x(), gl = plot_l(a.largura), gy = a.y + G2_PLOT_Y, ga = G2_A;
  tab5_screen_clear(a.x, a.y + G2_Y, a.largura, a.altura - G2_Y, TFT_BLACK);
  char buf[96], dur[24];

  fn(d, 28); d.setTextDatum(middle_left); d.setTextColor(COR_GASTO_TELA, TFT_BLACK);
  d.drawString("Gasto por hora (mAh)", a.x + M, a.y + G2_Y + 18);
  fc(d, 18);
  int xr = a.x + a.largura - M;
  quadradinho(d, &xr, a.y + G2_Y + 18, COR_CARGA, "carga");
  quadradinho(d, &xr, a.y + G2_Y + 18, COR_GASTO_REP, "repouso");
  quadradinho(d, &xr, a.y + G2_Y + 18, COR_GASTO_TELA, "tela");
  d.setTextDatum(top_left);

  if (!s_dia || !s_dia->t0) {
    d.drawFastHLine(gx, gy + ga - 1, gl, COR_EIXO);
    aviso_no_grafico(d, gx, gl, gy, ga, s_carga_pedida ? "reading the card..." : "needs the clock (GPS, mesh, or NTP)");
    return;
  }

  float mx = 0;
  for (int h = 0; h < 24; ++h) { const float g = s_m->horas[h].tela + s_m->horas[h].repouso; if (g > mx) { mx = g; } }
  float lo, hi, passo;
  escala(0, mx < 20.0f ? 20.0f : mx, 20.0f, &lo, &hi, &passo);
  lo = 0;
  desenhar_eixo_y(d, gx, gl, gy, ga, lo, hi, passo, false);

  // Barras alinhadas com as colunas de hora do grafico 1 (mesmo gx/gl).
  for (int h = 0; h < 24; ++h) {
    const int c0 = gx + (gl * h) / 24, c1 = gx + (gl * (h + 1)) / 24;
    const int bx = c0 + 3, bl = c1 - c0 - 6;
    const hora_t* hr = &s_m->horas[h];
    if (h == s_hora_sel) { d.fillRect(c0 + 1, gy, c1 - c0 - 2, ga - 1, COR_CARTAO_SEL); }
    const int a_rep  = (int)(hr->repouso * (ga - 2) / hi + 0.5f);
    const int a_tela = (int)(hr->tela    * (ga - 2) / hi + 0.5f);
    const int base = gy + ga - 2;
    if (a_rep > 0)  { d.fillRect(bx, base - a_rep + 1, bl, a_rep, COR_GASTO_REP); }
    if (a_tela > 0) { d.fillRect(bx, base - a_rep - a_tela + 1, bl, a_tela, COR_GASTO_TELA); }
    if (hr->carga > 5.0f) { d.fillRoundRect(bx, gy + ga + 2, bl, G2_FAIXA - 2, 2, COR_CARGA); }
  }
  eixo_tempo(d, gx, gl, gy + G2_FAIXA, ga, true, false);

  // DETALHE: a hora tocada, ou os totais das 24 h.
  const int y1 = a.y + G2_DET_Y, y2 = y1 + 32;
  d.setTextDatum(top_left);
  if (s_hora_sel >= 0) {
    const hora_t* hr = &s_m->horas[s_hora_sel];
    struct tm lt; tab5_local_time(s_dia->t0 + (uint32_t)s_hora_sel * 3600u, &lt);
    snprintf(buf, sizeof(buf), "%dh - %dh: gastou %.0f mAh · carregou %.0f mAh",
             lt.tm_hour, (lt.tm_hour + 1) % 24, (double)(hr->tela + hr->repouso), (double)hr->carga);
    fn(d, 24); d.setTextColor(TFT_WHITE, TFT_BLACK); d.drawString(buf, a.x + M, y1);
    if (hr->tem) {
      snprintf(buf, sizeof(buf), "tela acesa %d min (%.0f mAh)   repouso %.0f mAh",
               hr->min_tela, (double)hr->tela, (double)hr->repouso);
    } else {
      snprintf(buf, sizeof(buf), "sem registro nesta hora (aparelho desligado)");
    }
    fc(d, 24); d.setTextColor(TFT_LIGHTGREY, TFT_BLACK); d.drawString(buf, a.x + M, y2);
  } else {
    snprintf(buf, sizeof(buf), "24 h: gastou %.0f mAh (%.2f Wh) · carregou %.0f mAh",
             (double)s_m->tot_gasto, (double)s_m->tot_wh, (double)s_m->tot_carga);
    fn(d, 24); d.setTextColor(TFT_WHITE, TFT_BLACK); d.drawString(buf, a.x + M, y1);
    duracao(dur, sizeof(dur), s_m->tot_min_tela / 60.0f);
    uint32_t falhas; registro_estado(nullptr, nullptr, &falhas);
    if (falhas) { snprintf(buf, sizeof(buf), "screen on %s   log: %lu card failures", dur, (unsigned long)falhas); }
    else        { snprintf(buf, sizeof(buf), "tela acesa %s   toque numa barra para ver a hora", dur); }
    fc(d, 24); d.setTextColor(falhas ? TFT_ORANGE : TFT_LIGHTGREY, TFT_BLACK); d.drawString(buf, a.x + M, y2);
  }
}

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  const auto estilo = d.getTextStyle();
  const lgfx::IFont* fonte = d.getFont();
  pintura_iniciar(&s_p);
  const bool tudo = pintura_completa(&s_p);

  // 24 H: recarrega quando o registro gravou amostra nova (a cada 5 min) ou
  // quando virou a hora (a janela anda uma coluna).
  uint32_t grav; registro_estado(nullptr, &grav, nullptr);
  if (grav != s_gravadas_lidas || (s_dia && janela_t0() != s_dia->t0)) { s_gravadas_lidas = grav; pedir_24h(); }

  if (tudo) { tab5_screen_clear(a.x, a.y, a.largura, a.altura, TFT_BLACK); }

  // Os graficos so mudam quando chega dado (anel a cada TAB5_HIST_SEG, cartao
  // a cada carga) ou quando um toque mudou a selecao.
  static int s_assin = -1;
  const int n = tab5_power_history(s_m->ram);
  const int assin = (int)s_dia_ver * 7919 + n * 1000003 + (n ? (int)(s_m->ram[n - 1].mA * 10) : 0);
  if (tudo || s_graf_sujo || assin != s_assin) {
    s_assin = assin;
    s_graf_sujo = false;
    montar_24h(&s_m->p24);
    montar_horas(&s_m->p24);
    montar_30(&s_m->p30);
    desenhar_grafico1(d, a);
    desenhar_grafico2(d, a);
  }
  desenhar_topo(d, a);   // depois: o cartao "Gasto 24 h" le o total que montar_horas acabou de fazer

  d.setFont(fonte);
  d.setTextStyle(estilo);
}

static void ao_tocar(int x, int y)
{
  const area_t a = sistema_area();
  const int cl = cart_l(a.largura);
  const int gx = plot_x(), gl = plot_l(a.largura);
  if (y >= HERO_Y && y < HERO_Y + HERO_A) {
    s_serie = SERIE_BATERIA; s_cursor = -1;
  }
  // Um cartao do indice: escolhe a serie do grafico 1 (o sexto so informa).
  else if (y >= CART_Y && y < TOPO_A && x >= M) {
    const int col = (x - M) / (cl + CART_V), lin = (y - CART_Y) / (CART_A + CART_V);
    const int i = lin * 3 + (col > 2 ? 2 : col);
    if (i >= N_SERIES) { return; }
    s_serie = (serie_t)i; s_cursor = -1;
  }
  // O controle de periodo: metade esquerda 30 MIN, direita 24 H.
  else if (y >= G1_Y && y < G1_Y + SEG_A && x >= seg_x(a.largura)) {
    s_24h = (x >= seg_x(a.largura) + SEG_L / 2); s_cursor = -1;
    if (s_24h) { pedir_24h(); }
  }
  // Dentro do grafico 1: poe o cursor; tocar perto dele de novo tira.
  else if (y >= G1_PLOT_Y && y < G1_PLOT_Y + G1_A && x >= gx - 8 && x < gx + gl + 8) {
    s_cursor = (s_cursor >= 0 && abs(x - s_cursor) < 16) ? -1 : x;
  }
  // Uma barra do grafico 2 (com a faixa de carga e os rotulos embaixo).
  else if (y >= G2_PLOT_Y && y < G2_PLOT_Y + G2_A + G2_FAIXA + 34 && x >= gx && x < gx + gl) {
    const int h = (x - gx) * 24 / gl;
    s_hora_sel = (h == s_hora_sel) ? -1 : h;
  }
  else { return; }
  s_graf_sujo = true;
  sistema_pedir_redesenho();
}


// ---------------------------------------------------------------------------
// ICONE DA TELA INICIAL
// ---------------------------------------------------------------------------
// Primitivas e fracao do lado, como a marca do Mesh: sai limpo em qualquer
// tamanho, nao gasta flash e nao serrilha. A letra grande que estava aqui era
// a fonte embutida ampliada 6x - cada pixel dela virava um quadrado de 6.
static void icone_energia(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t fundo  = d.color565(255, 150, 40);
  const uint16_t escuro = d.color565(55, 30, 0);

  d.fillRoundRect(x, y, lado, lado, lado / 6, fundo);

  #define IX(f) (x + (int)((f) * lado))
  #define IY(f) (y + (int)((f) * lado))

  // Stack deitada com o polo a direita. Contorno grosso desenhado como dois
  // retangulos (cheio e vazado), que e mais barato que quatro linhas e nao
  // deixa canto aberto.
  d.fillRoundRect(IX(0.14f), IY(0.32f), (int)(0.62f * lado), (int)(0.36f * lado),
                  lado / 16, escuro);
  d.fillRoundRect(IX(0.14f) + lado / 22, IY(0.32f) + lado / 22,
                  (int)(0.62f * lado) - lado / 11, (int)(0.36f * lado) - lado / 11,
                  lado / 20, fundo);
  d.fillRoundRect(IX(0.78f), IY(0.42f), (int)(0.07f * lado), (int)(0.16f * lado),
                  lado / 40, escuro);

  // Raio dentro: e o que separa "bateria" de "consumo". O app mede corrente,
  // nao so carga.
  d.fillTriangle(IX(0.48f), IY(0.36f), IX(0.30f), IY(0.53f), IX(0.44f), IY(0.53f), escuro);
  d.fillTriangle(IX(0.40f), IY(0.64f), IX(0.58f), IY(0.47f), IX(0.44f), IY(0.47f), escuro);

  #undef IX
  #undef IY
}

extern const app_t APP_ENERGY = {
  .name  = "Energy",
  .cor   = TFT_ORANGE,
  // Perfil: so o brilho. Todo o resto vem dos defaults do tab5_profile_t,
  // que sao 'desligado' - por isso nao precisa listar o que nao se usa.
  .perfil = { },
  .piso_repouso_mhz = 0,  // sem trabalho de fundo: pode descer ao minimo
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
  .periodo_ms = 1000,     // o topo acompanha a medida de 1Hz do kernel
  .avisos = AVISO_TRABALHO,
  .ao_aviso = ao_aviso,
  .icone = icone_energia,
};
