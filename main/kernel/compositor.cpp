#include "compositor.hpp"

#include "tab5_screen.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <atomic>

static const char *TAG = "comp";

static const int FB_L = 720;
static const int FB_A = 1280;

// ---------------------------------------------------------------------------
// AREAS SUJAS
// ---------------------------------------------------------------------------
// Fundir duas areas so vale quando o retangulo que as envolve nao e maior que
// as duas somadas - senao redesenhamos pixel que ninguem pediu. E a mesma
// regra da LVGL (lv_refr.c, refr_join_area).
//
// 32 e nao 16: uma tela com texto espalhado (barra de cima, valores no meio,
// barra de baixo) enche 16 com facilidade, e o que acontece ao encher custa
// caro - ver estourou().
//
// 96 e nao 32: ANIMACAO COM MUITOS OBJETOS PEQUENOS. Particulas, flocos,
// estrelas - cada um e uma caixinha, e 32 enchiam no meio de uma explosao de
// fogos. Cheia, a lista junta caixas distantes em retangulos gordos, e a
// pagina seguinte copia toda a sobra: o quadro pesava justo quando a tela
// enchia, e a animacao engasgava. Quem anima tinha de juntar as proprias
// caixas para caber aqui - cuidado que nao e de app. Custa 1,5 KB de RAM
// interna a mais (tres listas); o caminho de cada escrita continua curto
// (ver s_ult_contem).
#define MAX_AREAS 96

typedef struct { tab5_rect_t r[MAX_AREAS]; int n; } areas_t;

static areas_t s_sujas;      // o que vai ser desenhado no proximo frame
static areas_t s_anterior;   // o que o frame anterior desenhou (falta na pagina que saiu)
static uint32_t s_estouros = 0;

static inline int area(const tab5_rect_t& r) { return r.l * r.a; }

static tab5_rect_t envolver(const tab5_rect_t& a, const tab5_rect_t& b)
{
  const int x0 = a.x < b.x ? a.x : b.x;
  const int y0 = a.y < b.y ? a.y : b.y;
  const int x1 = (a.x + a.l) > (b.x + b.l) ? (a.x + a.l) : (b.x + b.l);
  const int y1 = (a.y + a.a) > (b.y + b.a) ? (a.y + a.a) : (b.y + b.a);
  return { (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
}

// Quanto de pixel NAO pedido a fusao de a e b obriga a copiar.
static inline int desperdicio(const tab5_rect_t& a, const tab5_rect_t& b)
{
  return area(envolver(a, b)) - area(a) - area(b);
}

static bool contem(const tab5_rect_t& fora, const tab5_rect_t& dentro)
{
  return dentro.x >= fora.x && dentro.y >= fora.y
      && dentro.x + dentro.l <= fora.x + fora.l
      && dentro.y + dentro.a <= fora.y + fora.a;
}

static bool recortar_na_tela(tab5_rect_t& r)
{
  int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
  int x1 = r.x + r.l > FB_L ? FB_L : r.x + r.l;
  int y1 = r.y + r.a > FB_A ? FB_A : r.y + r.a;
  if (x1 <= x0 || y1 <= y0) { return false; }
  r = { (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
  return true;
}

// A area que cobriu a ultima escrita. Quase toda escrita cai dentro da mesma
// area que a anterior (as linhas de um circulo, os glifos de um texto, o
// desenho por cima da caixa que acabou de ser apagada): olhar ela primeiro
// poupa varrer a lista inteira, que agora e longa.
static int s_ult_contem = 0;

void comp_invalidar(const tab5_rect_t& entrada)
{
  tab5_rect_t r = entrada;
  if (!recortar_na_tela(r)) { return; }
  if (s_ult_contem < s_sujas.n && contem(s_sujas.r[s_ult_contem], r)) { return; }

  for (;;) {
    // Ja coberta: nada a fazer.
    for (int i = 0; i < s_sujas.n; ++i) { if (contem(s_sujas.r[i], r)) { s_ult_contem = i; return; } }

    // Funde enquanto compensar. Uma fusao pode habilitar outra, entao repete.
    bool fundiu = true;
    while (fundiu) {
      fundiu = false;
      for (int i = 0; i < s_sujas.n; ++i) {
        if (desperdicio(s_sujas.r[i], r) <= 0) {
          r = envolver(s_sujas.r[i], r);
          s_sujas.r[i] = s_sujas.r[--s_sujas.n];
          fundiu = true;
          break;
        }
      }
    }

    if (s_sujas.n < MAX_AREAS) { s_sujas.r[s_sujas.n++] = r; return; }

    // LISTA CHEIA. A primeira versao envolvia TUDO num retangulo so - e a
    // barra de cima mais um numero la embaixo viravam a tela inteira (1,8 MB
    // de copia por um punhado de texto). Agora a area nova se junta a VIZINHA
    // que desperdica menos: abre espaco sem engordar o que esta longe. (A
    // versao anterior procurava o melhor PAR entre todas - n^2 a cada estouro,
    // caro demais com a lista longa; com 96 vagas o estouro ficou raro e a
    // diferenca de sobra entre os dois jeitos, pequena.)
    ++s_estouros;
    int bi = 0;
    int melhor = 0x7fffffff;
    for (int i = 0; i < s_sujas.n; ++i) {
      const int dr = desperdicio(s_sujas.r[i], r);
      if (dr < melhor) { melhor = dr; bi = i; }
    }
    r = envolver(s_sujas.r[bi], r);
    s_sujas.r[bi] = s_sujas.r[--s_sujas.n];
    // Volta ao topo: a area fundida pode agora cobrir ou fundir com outras.
  }
}

// ---------------------------------------------------------------------------
// ESTADO DO QUADRO
// ---------------------------------------------------------------------------
static bool     s_aberto   = false;   // a pagina de tras esta sincronizada e em uso
static bool     s_ativo    = false;   // conhecemos as paginas do painel atual
static bool     s_pendente = false;   // pedimos troca e ainda nao confirmamos
static uint32_t s_geracao  = 0;       // de qual painel sao as paginas que conhecemos
static uint32_t s_geracao_vista = 0;  // ultimo painel anunciado ao core (comp_painel_novo)
static void*    s_tras     = nullptr; // a pagina de tras do quadro aberto
static uint32_t s_epoca    = 0;       // g_tab5_screen_epoch quando o quadro abriu
static uint32_t s_rodada   = 0;
static int64_t  s_ult_quadro_us = 0;  // diagnostico: quando saiu o ultimo quadro
static uint32_t s_ult_quadro_px = 0;  // e quantos pixels ele mudou
static std::atomic<bool> s_pedido_tudo{false};

// SINCRONIA PREGUICOSA. As areas do quadro anterior que a pagina de tras
// ainda NAO tem. Nao sao copiadas ao abrir: cada uma vem da frente so quando
// algo vai tocar nela (escrita ou leitura - comp_preparar), e o que sobrar,
// na hora de apresentar. A que for inteira sobrescrita por uma copia de DMA
// (rolagem, superficie) nunca e copiada.
//
// Por que: a primeira versao sincronizava TUDO ao abrir. Mas o quadro abre
// na primeira escrita da volta - e os apps desenham o cabecalho antes da
// lista. Quando a rolagem chegava dizendo "vou cobrir a janela inteira", a
// janela ja tinha sido copiada: medido, a sincronia era 2/3 de toda a copia.
static areas_t s_pend;
static void*   s_frente_pg = nullptr;   // a pagina da frente do quadro aberto

static comp_estat_t s_est = {};       // a medida (ver comp_estat)
static uint64_t     s_us_dentro = 0;  // tempo gasto aqui dentro (abrir + copias)
static uint64_t     s_marca_dentro = 0;

static bool intersecta(const tab5_rect_t& a, const tab5_rect_t& b)
{
  return a.x < b.x + b.l && b.x < a.x + a.l && a.y < b.y + b.a && b.y < a.y + a.a;
}

// Toda copia do compositor passa por aqui: e onde o tempo de DMA e contado.
static bool copiar(void* destino, const tab5_rect_t& r, const void* origem, int ox, int oy)
{
  const int64_t t0 = esp_timer_get_time();
  const bool ok = tab5_frame_copy_from(destino, r, origem, ox, oy);
  const uint64_t dt = (uint64_t)(esp_timer_get_time() - t0);
  s_est.us_copia += dt;
  s_us_dentro += dt;
  return ok;
}

static void esperar_troca_pendente(void)
{
  if (!s_pendente) { return; }
  const int64_t t0 = esp_timer_get_time();
  if (!tab5_frame_wait_for_swap(50)) { ESP_LOGW(TAG, "troca nao confirmada em 50 ms"); }
  const uint64_t dt = (uint64_t)(esp_timer_get_time() - t0);
  s_est.us_espera += dt;
  s_us_dentro += dt;
  s_pendente = false;
}

// Descarta o quadro em andamento: o painel sumiu (repouso) ou foi trocado.
// O que estava desenhado se perde de proposito - o core redesenha tudo
// quando o painel novo aparecer (comp_painel_novo).
static void descartar(void)
{
  s_aberto = false;
  s_tras = nullptr;
  s_frente_pg = nullptr;
  s_sujas.n = 0;
  s_pend.n = 0;
  // Rodada nova: o atalho do rastreador ("este retangulo ja foi anotado e
  // preparado nesta rodada") nao pode atravessar para o quadro seguinte - ele
  // pularia o comp_preparar, e a sincronia da apresentacao apagaria o desenho.
  ++s_rodada;
}

// AS PENDENTES SAO RECORTADAS, nao copiadas inteiras. A fusao de areas junta
// vizinhos (o cabecalho de um app e a janela da lista logo abaixo viram UM
// retangulo pendente). Copiar a pendente inteira porque o cabecalho foi
// tocado trazia a janela junto - 1,6 MB que a rolagem sobrescreve em seguida.
// Agora so a INTERSECAO vem (ou e descartada), e o resto da pendente continua
// pendente, em ate 4 pedacos.
static tab5_rect_t intersecao(const tab5_rect_t& a, const tab5_rect_t& b)
{
  const int x0 = a.x > b.x ? a.x : b.x;
  const int y0 = a.y > b.y ? a.y : b.y;
  const int x1 = (a.x + a.l) < (b.x + b.l) ? (a.x + a.l) : (b.x + b.l);
  const int y1 = (a.y + a.a) < (b.y + b.a) ? (a.y + a.a) : (b.y + b.a);
  return { (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
}

// Tira a pendente i e devolve a lista o que dela fica FORA de 'r' (faixa de
// cima, de baixo, esquerda, direita). false = nao coube na lista: nada foi
// mexido, e quem chamou copia a pendente inteira.
static bool recortar_pendente(int i, const tab5_rect_t& r)
{
  const tab5_rect_t p = s_pend.r[i];
  const tab5_rect_t c = intersecao(p, r);
  tab5_rect_t pedaco[4];
  int n = 0;
  if (c.y > p.y)                 { pedaco[n++] = { p.x, p.y, p.l, (int16_t)(c.y - p.y) }; }
  if (c.y + c.a < p.y + p.a)     { pedaco[n++] = { p.x, (int16_t)(c.y + c.a), p.l, (int16_t)(p.y + p.a - c.y - c.a) }; }
  if (c.x > p.x)                 { pedaco[n++] = { p.x, c.y, (int16_t)(c.x - p.x), c.a }; }
  if (c.x + c.l < p.x + p.l)     { pedaco[n++] = { (int16_t)(c.x + c.l), c.y, (int16_t)(p.x + p.l - c.x - c.l), c.a }; }
  if (s_pend.n - 1 + n > MAX_AREAS) { return false; }
  s_pend.r[i] = s_pend.r[--s_pend.n];
  for (int k = 0; k < n; ++k) { s_pend.r[s_pend.n++] = pedaco[k]; }
  return true;
}

static void copiar_pendente(const tab5_rect_t& a)
{
  copiar(s_tras, a, s_frente_pg, a.x, a.y);
  s_est.px_sinc += (uint32_t)area(a);
}

// Traz a pendente i inteira e a tira da lista (troca com a ultima - quem
// percorre de tras para frente nao perde ninguem).
static void sincronizar(int i)
{
  const tab5_rect_t a = s_pend.r[i];
  s_pend.r[i] = s_pend.r[--s_pend.n];
  copiar_pendente(a);
}

// Percorre de tras para frente: os pedacos acrescentados no fim ficam FORA
// de 'r' e nao precisam ser revistos.
void comp_preparar(const tab5_rect_t& r)
{
  if (!s_aberto || s_pend.n == 0) { return; }
  for (int i = s_pend.n - 1; i >= 0; --i) {
    if (i >= s_pend.n || !intersecta(s_pend.r[i], r)) { continue; }
    const tab5_rect_t c = intersecao(s_pend.r[i], r);
    if (recortar_pendente(i, r)) { copiar_pendente(c); }
    else                         { sincronizar(i); }
  }
}

void comp_cobrir(const tab5_rect_t& r)
{
  if (!s_aberto) { return; }
  for (int i = s_pend.n - 1; i >= 0; --i) {
    if (i >= s_pend.n || !intersecta(s_pend.r[i], r)) { continue; }
    if (contem(r, s_pend.r[i])) { s_pend.r[i] = s_pend.r[--s_pend.n]; continue; }
    if (!recortar_pendente(i, r)) { sincronizar(i); }   // sem vaga: vem inteira (correto, so mais caro)
  }
}

void* comp_abrir(void)
{
  if (s_aberto) {
    // O CAMINHO DE TODA ESCRITA (cada pixel de borda de letra passa aqui):
    // uma comparacao, nada mais. Um quadro aberto nao pode atravessar a
    // troca do painel (repouso, relogio) - a pagina dele foi liberada junto
    // com o painel antigo -, e a epoca das paginas sobe exatamente nisso.
    if (s_epoca == g_tab5_screen_epoch) { return s_tras; }
    descartar();
  }

  // Sem painel (repouso): nada a abrir.
  if (!tab5_screen_init()) { return nullptr; }

  // PAINEL NOVO (repouso, troca de relogio, boot): as paginas que
  // conheciamos nao existem mais, e as novas nao tem nada nosso - nao ha o
  // que sincronizar. O core ja foi (ou vai ser) avisado por
  // comp_painel_novo() e redesenha a tela inteira.
  if (!s_ativo || tab5_screen_generation() != s_geracao) {
    s_geracao = tab5_screen_generation();
    s_ativo = true;
    s_pendente = false;
    s_anterior.n = 0;
  }

  // 1. A pagina de tras so esta livre depois que o painel trocou.
  esperar_troca_pendente();
  void* tras   = tab5_screen_page(true);
  void* frente = tab5_screen_page(false);
  if (!tras || !frente) { return nullptr; }

  // 2. Sincronia, PREGUICOSA: o que o quadro anterior desenhou fica
  //    pendente, e vem da frente so quando for tocado (ver s_pend).
  s_pend = s_anterior;
  s_anterior.n = 0;

  s_tras = tras;
  s_frente_pg = frente;
  s_epoca = g_tab5_screen_epoch;
  s_aberto = true;
  return s_tras;
}

bool comp_desenhado_neste_quadro(const tab5_rect_t& src)
{
  for (int i = 0; i < s_sujas.n; ++i) { if (intersecta(s_sujas.r[i], src)) { return true; } }
  return false;
}

bool comp_copiar_da_frente(const tab5_rect_t& r, int ox, int oy)
{
  void* tras = comp_abrir();
  if (!tras) { return false; }
  if (!s_frente_pg || s_frente_pg == tras) { return false; }
  if (!copiar(tras, r, s_frente_pg, ox, oy)) { return false; }
  comp_cobrir(r);   // so DEPOIS de dar certo: falhou, as pendentes continuam
  s_est.px_rolar_dma += (uint32_t)area(r);
  return true;
}

bool comp_copiar_imagem(const void* imagem, const tab5_rect_t& r)
{
  void* tras = comp_abrir();
  if (!tras || !imagem) { return false; }
  if (!copiar(tras, r, imagem, r.x, r.y)) { return false; }
  comp_cobrir(r);
  comp_invalidar(r);
  return true;
}

void comp_invalidar_tudo(void) { s_pedido_tudo.store(true, std::memory_order_relaxed); }

bool comp_apresentar(void)
{
  if (s_pedido_tudo.exchange(false, std::memory_order_relaxed)) {
    comp_invalidar({ 0, 0, (int16_t)FB_L, (int16_t)FB_A });
  }
  if (s_sujas.n == 0) { return false; }

  // Marcado sem nada desenhado (so um pedido de invalidar): abre aqui, para
  // a pagina de tras ficar igual a da frente antes de ir ao ar.
  if (!s_aberto && !comp_abrir()) { descartar(); return false; }

  // O painel sumiu ou foi trocado DEPOIS de abrir: a pagina em que
  // desenhamos nao existe mais.
  if (!tab5_screen_init() || tab5_screen_generation() != s_geracao) { descartar(); return false; }

  // O que ninguem tocou continua pendente: vem agora, senao a pagina de tras
  // iria ao ar com o quadro de duas trocas atras nesses pontos.
  while (s_pend.n > 0) { sincronizar(s_pend.n - 1); }

  int y_min = FB_A, y_max = 0;
  uint32_t px = 0;
  for (int k = 0; k < s_sujas.n; ++k) {
    const tab5_rect_t& a = s_sujas.r[k];
    if (a.y < y_min) { y_min = a.y; }
    if (a.y + a.a > y_max) { y_max = a.y + a.a; }
    px += (uint32_t)area(a);
  }

  // 3. A troca. O driver devolve ao cache as linhas y_min..y_max da pagina
  //    de tras (o que a CPU escreveu e ainda esta so no cache) e muda o
  //    indice. Se recusar, o quadro fica aberto e tenta de novo na proxima
  //    volta - fechar aqui faria a sincronia apagar o que foi desenhado.
  if (!tab5_screen_request_swap(y_min, y_max)) { return false; }
  s_pendente = true;
  s_anterior = s_sujas;
  s_sujas.n = 0;
  s_aberto = false;
  s_tras = nullptr;
  s_frente_pg = nullptr;
  ++s_rodada;

  s_ult_quadro_us = esp_timer_get_time();
  s_ult_quadro_px = px;
  ++s_est.frames;
  s_est.px_quadro += px;
  if (px >= (uint32_t)FB_L * FB_A / 10 * 9) { ++s_est.cheios; }
  return true;
}

bool comp_painel_novo(void)
{
  if (!tab5_screen_init()) { return false; }
  const uint32_t g = tab5_screen_generation();
  if (g == s_geracao_vista) { return false; }
  s_geracao_vista = g;
  return true;
}

bool comp_tem_sujeira(void) { return s_sujas.n > 0 || s_pedido_tudo.load(std::memory_order_relaxed); }
void comp_esperar_troca(void) { esperar_troca_pendente(); }
uint32_t comp_rodada(void) { return s_rodada; }

void comp_ultimo_quadro(int64_t* quando_us, uint32_t* px) { if (quando_us) { *quando_us = s_ult_quadro_us; } if (px) { *px = s_ult_quadro_px; } }

void comp_marcar_desenho(void) { s_marca_dentro = s_us_dentro; }

uint32_t comp_contar_desenho(uint32_t us, bool completo)
{
  const uint64_t dentro = s_us_dentro - s_marca_dentro;
  const uint64_t cpu = (us > dentro) ? (us - dentro) : 0;
  s_est.us_desenho += cpu;
  ++s_est.n_desenho;
  if (completo) { s_est.us_desenho_completo += cpu; ++s_est.n_desenho_completo; }
  return (uint32_t)cpu;
}

void comp_contar_volta(uint32_t us)
{
  // O periodo MEDIDO do painel (hal/tab5_frame), e nao um numero fixo: o
  // relogio do painel e ajuste do usuario (59 MHz = 20,6 ms, 80 MHz = 15,7).
  uint32_t periodo = 0;
  tab5_frame_delays(nullptr, &periodo);
  if (periodo == 0) { periodo = 16667; }
  uint32_t f = us / periodo;          // 0 = coube em 1 periodo
  if (f > COMP_FAIXAS - 1) { f = COMP_FAIXAS - 1; }
  s_est.us_volta += us;
  ++s_est.voltas[f];
}

// Leitura de outra tarefa (console, monitor) sem trava: campos de 64 bits
// podem sair rasgados num instante raro. E diagnostico - um numero torto numa
// janela some na seguinte; uma trava aqui pesaria em todo quadro.
void comp_estat(comp_estat_t* out)
{
  if (!out) { return; }
  *out = s_est;
  out->estouros = s_estouros;
  tab5_screen_scroll_cost(&out->px_rolados, &out->us_rolar);
}
