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
// Fundir duas areas so vale when o rectangle que as envolve nao e maior que
// as duas somadas - senao redesenhamos pixel que ninguem pediu. E a mesma
// regra of LVGL (lv_refr.c, refr_join_area).
//
// 32 e nao 16: uma screen com text espalhado (barra of top, valores no middle,
// barra of bottom) enche 16 com facilidade, e o que acontece on encher custa
// caro - ver estourou().
//
// 96 e nao 32: ANIMACAO COM MUITOS OBJETOS PEQUENOS. Particulas, flocos,
// estrelas - cada one e uma caixinha, e 32 enchiam no middle of uma explosao of
// fogos. Cheia, a lista junta caixas distantes em retangulos gordos, e a
// pagina seguinte copia toda a sobra: o frame pesava justo when a screen
// enchia, e a animacao engasgava. Quem anima tinha of juntar as proprias
// caixas para caber aqui - cuidado que nao e of app. Custa 1,5 KB of RAM
// interna a mais (tres listas); o caminho of cada escrita continua curto
// (ver s_ult_contem).
#define MAX_AREAS 96

typedef struct { tab5_rect_t r[MAX_AREAS]; int n; } areas_t;

static areas_t s_sujas;      // o que vai ser drawn no next frame
static areas_t s_anterior;   // o que o frame anterior desenhou (falta in pagina que exited)
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

// Quanto of pixel NAO request a fusao of a e b obriga a copiar.
static inline int desperdicio(const tab5_rect_t& a, const tab5_rect_t& b)
{
  return area(envolver(a, b)) - area(a) - area(b);
}

static bool contains(const tab5_rect_t& fora, const tab5_rect_t& dentro)
{
  return dentro.x >= fora.x && dentro.y >= fora.y
      && dentro.x + dentro.l <= fora.x + fora.l
      && dentro.y + dentro.a <= fora.y + fora.a;
}

static bool recortar_in_screen(tab5_rect_t& r)
{
  int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
  int x1 = r.x + r.l > FB_L ? FB_L : r.x + r.l;
  int y1 = r.y + r.a > FB_A ? FB_A : r.y + r.a;
  if (x1 <= x0 || y1 <= y0) { return false; }
  r = { (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
  return true;
}

// A area que cobriu a ultima escrita. Quase toda escrita cai dentro of mesma
// area que a anterior (as lines of one circulo, os glifos of one text, o
// drawing por top of caixa que acabou of ser apagada): olhar ela first
// poupa varrer a lista inteira, que now e longa.
static int s_ult_contains = 0;

void comp_invalidar(const tab5_rect_t& entrada)
{
  tab5_rect_t r = entrada;
  if (!recortar_in_screen(r)) { return; }
  if (s_ult_contains < s_sujas.n && contains(s_sujas.r[s_ult_contains], r)) { return; }

  for (;;) {
    // Ja coberta: nada a fazer.
    for (int i = 0; i < s_sujas.n; ++i) { if (contains(s_sujas.r[i], r)) { s_ult_contains = i; return; } }

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

    // LISTA CHEIA. A primeira versao envolvia TUDO num rectangle so - e a
    // barra of top mais one numero la embaixo viravam a screen inteira (1,8 MB
    // of copia por one punhado of text). Agora a area nova se junta a VIZINHA
    // que desperdica menos: abre espaco sem engordar o que esta longe. (A
    // versao anterior procurava o melhor PAR entre todas - n^2 a cada estouro,
    // caro demais com a lista longa; com 96 vagas o estouro became raro e a
    // diferenca of sobra entre os dois jeitos, pequena.)
    ++s_estouros;
    int bi = 0;
    int melhor = 0x7fffffff;
    for (int i = 0; i < s_sujas.n; ++i) {
      const int dr = desperdicio(s_sujas.r[i], r);
      if (dr < melhor) { melhor = dr; bi = i; }
    }
    r = envolver(s_sujas.r[bi], r);
    s_sujas.r[bi] = s_sujas.r[--s_sujas.n];
    // Volta on topo: a area fundida pode now cover ou fundir com outras.
  }
}

// ---------------------------------------------------------------------------
// ESTADO DO QUADRO
// ---------------------------------------------------------------------------
static bool     s_open   = false;   // a pagina of tras esta sincronizada e em uso
static bool     s_active    = false;   // conhecemos as paginas of panel current
static bool     s_pending = false;   // pedimos troca e ainda nao confirmamos
static uint32_t s_geracao  = 0;       // of qual panel sao as paginas que conhecemos
static uint32_t s_geracao_vista = 0;  // ultimo panel anunciado on core (comp_painel_novo)
static void*    s_tras     = nullptr; // a pagina of tras of frame open
static uint32_t s_epoca    = 0;       // g_tab5_screen_epoch when o frame abriu
static uint32_t s_round   = 0;
static int64_t  s_ult_frame_us = 0;  // diagnostics: when exited o ultimo frame
static uint32_t s_ult_frame_px = 0;  // e quantos pixels ele changed
static std::atomic<bool> s_request_everything{false};

// SINCRONIA PREGUICOSA. As areas of frame anterior que a pagina of tras
// ainda NAO tem. Nao sao copiadas on open: cada uma vem of frente so when
// algo vai touch nela (escrita ou leitura - comp_preparar), e o que sobrar,
// in hora of apresentar. A que for inteira sobrescrita por uma copia of DMA
// (rolagem, surface) nunca e copiada.
//
// Por que: a primeira versao sincronizava TUDO on open. Mas o frame abre
// in primeira escrita of iteration - e os apps desenham o cabecalho antes of
// lista. Quando a rolagem chegava dizendo "vou cover a window inteira", a
// window ja tinha sido copiada: measured, a sincronia era 2/3 of toda a copia.
static areas_t s_pend;
static void*   s_frente_pg = nullptr;   // a pagina of frente of frame open

static comp_estat_t s_est = {};       // a measurement (ver comp_estat)
static uint64_t     s_us_dentro = 0;  // time gasto aqui dentro (open + copias)
static uint64_t     s_marca_dentro = 0;

static bool intersecta(const tab5_rect_t& a, const tab5_rect_t& b)
{
  return a.x < b.x + b.l && b.x < a.x + a.l && a.y < b.y + b.a && b.y < a.y + a.a;
}

// Toda copia of compositor passa por aqui: e onde o time of DMA e contado.
static bool copiar(void* destino, const tab5_rect_t& r, const void* origem, int ox, int oy)
{
  const int64_t t0 = esp_timer_get_time();
  const bool ok = tab5_frame_copy_from(destino, r, origem, ox, oy);
  const uint64_t dt = (uint64_t)(esp_timer_get_time() - t0);
  s_est.us_copia += dt;
  s_us_dentro += dt;
  return ok;
}

static void wait_troca_pending(void)
{
  if (!s_pending) { return; }
  const int64_t t0 = esp_timer_get_time();
  if (!tab5_frame_wait_for_swap(50)) { ESP_LOGW(TAG, "troca nao confirmada em 50 ms"); }
  const uint64_t dt = (uint64_t)(esp_timer_get_time() - t0);
  s_est.us_espera += dt;
  s_us_dentro += dt;
  s_pending = false;
}

// Descarta o frame em andamento: o panel sumiu (sleep) ou foi trocado.
// O que estava drawn se perde of proposito - o core redesenha everything
// when o panel novo aparecer (comp_painel_novo).
static void descartar(void)
{
  s_open = false;
  s_tras = nullptr;
  s_frente_pg = nullptr;
  s_sujas.n = 0;
  s_pend.n = 0;
  // Rodada nova: o atalho of rastreador ("este rectangle ja foi anotado e
  // preparado nesta round") nao pode atravessar para o frame seguinte - ele
  // pularia o comp_preparar, e a sincronia of apresentacao apagaria o drawing.
  ++s_round;
}

// AS PENDENTES SAO RECORTADAS, nao copiadas inteiras. A fusao of areas junta
// vizinhos (o cabecalho of one app e a window of lista logo abaixo viram UM
// rectangle pending). Copiar a pending inteira porque o cabecalho foi
// touched trazia a window junto - 1,6 MB que a rolagem sobrescreve em seguida.
// Agora so a INTERSECAO vem (ou e descartada), e o resto of pending continua
// pending, em ate 4 pedacos.
static tab5_rect_t intersecao(const tab5_rect_t& a, const tab5_rect_t& b)
{
  const int x0 = a.x > b.x ? a.x : b.x;
  const int y0 = a.y > b.y ? a.y : b.y;
  const int x1 = (a.x + a.l) < (b.x + b.l) ? (a.x + a.l) : (b.x + b.l);
  const int y1 = (a.y + a.a) < (b.y + b.a) ? (a.y + a.a) : (b.y + b.a);
  return { (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
}

// Tira a pending i e devolve a lista o que dela stays FORA of 'r' (faixa of
// top, of bottom, left, right). false = nao coube in lista: nada foi
// mexido, e quem chamou copia a pending inteira.
static bool recortar_pending(int i, const tab5_rect_t& r)
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

static void copiar_pending(const tab5_rect_t& a)
{
  copiar(s_tras, a, s_frente_pg, a.x, a.y);
  s_est.px_sinc += (uint32_t)area(a);
}

// Traz a pending i inteira e a tira of lista (troca com a ultima - quem
// percorre of tras para frente nao perde ninguem).
static void synchronize(int i)
{
  const tab5_rect_t a = s_pend.r[i];
  s_pend.r[i] = s_pend.r[--s_pend.n];
  copiar_pending(a);
}

// Percorre of tras para frente: os pedacos acrescentados no end ficam FORA
// of 'r' e nao precisam ser revistos.
void comp_preparar(const tab5_rect_t& r)
{
  if (!s_open || s_pend.n == 0) { return; }
  for (int i = s_pend.n - 1; i >= 0; --i) {
    if (i >= s_pend.n || !intersecta(s_pend.r[i], r)) { continue; }
    const tab5_rect_t c = intersecao(s_pend.r[i], r);
    if (recortar_pending(i, r)) { copiar_pending(c); }
    else                         { synchronize(i); }
  }
}

void comp_cobrir(const tab5_rect_t& r)
{
  if (!s_open) { return; }
  for (int i = s_pend.n - 1; i >= 0; --i) {
    if (i >= s_pend.n || !intersecta(s_pend.r[i], r)) { continue; }
    if (contains(r, s_pend.r[i])) { s_pend.r[i] = s_pend.r[--s_pend.n]; continue; }
    if (!recortar_pending(i, r)) { synchronize(i); }   // sem vaga: vem inteira (correto, so mais caro)
  }
}

void* comp_abrir(void)
{
  if (s_open) {
    // O CAMINHO DE TODA ESCRITA (cada pixel of borda of letra passa aqui):
    // uma comparacao, nada mais. Um frame open nao pode atravessar a
    // troca of panel (sleep, clock) - a pagina dele foi liberada junto
    // com o panel antigo -, e a epoca das paginas sobe exatamente nisso.
    if (s_epoca == g_tab5_screen_epoch) { return s_tras; }
    descartar();
  }

  // Sem panel (sleep): nada a open.
  if (!tab5_screen_init()) { return nullptr; }

  // PAINEL NOVO (sleep, troca of clock, boot): as paginas que
  // conheciamos nao existem mais, e as novas nao tem nada nosso - nao ha o
  // que synchronize. O core ja foi (ou vai ser) avisado por
  // comp_painel_novo() e redesenha a screen inteira.
  if (!s_active || tab5_screen_generation() != s_geracao) {
    s_geracao = tab5_screen_generation();
    s_active = true;
    s_pending = false;
    s_anterior.n = 0;
  }

  // 1. A pagina of tras so esta livre depois que o panel trocou.
  wait_troca_pending();
  void* tras   = tab5_screen_page(true);
  void* frente = tab5_screen_page(false);
  if (!tras || !frente) { return nullptr; }

  // 2. Sincronia, PREGUICOSA: o que o frame anterior desenhou stays
  //    pending, e vem of frente so when for touched (ver s_pend).
  s_pend = s_anterior;
  s_anterior.n = 0;

  s_tras = tras;
  s_frente_pg = frente;
  s_epoca = g_tab5_screen_epoch;
  s_open = true;
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
  comp_cobrir(r);   // so DEPOIS of dar certo: falhou, as pendentes continuam
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

void comp_invalidar_tudo(void) { s_request_everything.store(true, std::memory_order_relaxed); }

bool comp_apresentar(void)
{
  if (s_request_everything.exchange(false, std::memory_order_relaxed)) {
    comp_invalidar({ 0, 0, (int16_t)FB_L, (int16_t)FB_A });
  }
  if (s_sujas.n == 0) { return false; }

  // Marcado sem nada drawn (so one request of invalidate): abre aqui, para
  // a pagina of tras ficar igual a of frente antes of ir on ar.
  if (!s_open && !comp_abrir()) { descartar(); return false; }

  // O panel sumiu ou foi trocado DEPOIS of open: a pagina em que
  // desenhamos nao existe mais.
  if (!tab5_screen_init() || tab5_screen_generation() != s_geracao) { descartar(); return false; }

  // O que ninguem tocou continua pending: vem now, senao a pagina of tras
  // iria on ar com o frame of duas trocas atras nesses pontos.
  while (s_pend.n > 0) { synchronize(s_pend.n - 1); }

  int y_min = FB_A, y_max = 0;
  uint32_t px = 0;
  for (int k = 0; k < s_sujas.n; ++k) {
    const tab5_rect_t& a = s_sujas.r[k];
    if (a.y < y_min) { y_min = a.y; }
    if (a.y + a.a > y_max) { y_max = a.y + a.a; }
    px += (uint32_t)area(a);
  }

  // 3. A troca. O driver devolve on cache as lines y_min..y_max of pagina
  //    of tras (o que a CPU escreveu e ainda esta so no cache) e muda o
  //    indice. Se recusar, o frame stays open e tenta of novo in next
  //    iteration - close aqui faria a sincronia erase o que foi drawn.
  if (!tab5_screen_request_swap(y_min, y_max)) { return false; }
  s_pending = true;
  s_anterior = s_sujas;
  s_sujas.n = 0;
  s_open = false;
  s_tras = nullptr;
  s_frente_pg = nullptr;
  ++s_round;

  s_ult_frame_us = esp_timer_get_time();
  s_ult_frame_px = px;
  ++s_est.frames;
  s_est.px_frame += px;
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

bool comp_tem_sujeira(void) { return s_sujas.n > 0 || s_request_everything.load(std::memory_order_relaxed); }
void comp_esperar_troca(void) { wait_troca_pending(); }
uint32_t comp_rodada(void) { return s_round; }

void comp_ultimo_quadro(int64_t* when_us, uint32_t* px) { if (when_us) { *when_us = s_ult_frame_us; } if (px) { *px = s_ult_frame_px; } }

void comp_marcar_desenho(void) { s_marca_dentro = s_us_dentro; }

uint32_t comp_contar_desenho(uint32_t us, bool complete)
{
  const uint64_t dentro = s_us_dentro - s_marca_dentro;
  const uint64_t cpu = (us > dentro) ? (us - dentro) : 0;
  s_est.us_drawing += cpu;
  ++s_est.n_drawing;
  if (complete) { s_est.us_drawing_complete += cpu; ++s_est.n_drawing_complete; }
  return (uint32_t)cpu;
}

void comp_contar_volta(uint32_t us)
{
  // O periodo MEDIDO of panel (hal/tab5_frame), e nao one numero fixo: o
  // clock of panel e ajuste of user (59 MHz = 20,6 ms, 80 MHz = 15,7).
  uint32_t periodo = 0;
  tab5_frame_delays(nullptr, &periodo);
  if (periodo == 0) { periodo = 16667; }
  uint32_t f = us / periodo;          // 0 = coube em 1 periodo
  if (f > COMP_FAIXAS - 1) { f = COMP_FAIXAS - 1; }
  s_est.us_iteration += us;
  ++s_est.voltas[f];
}

// Leitura of outra tarefa (console, monitor) sem trava: campos of 64 bits
// podem exit rasgados num instante raro. E diagnostics - one numero torto numa
// window some in seguinte; uma trava aqui pesaria em todo frame.
void comp_estat(comp_estat_t* out)
{
  if (!out) { return; }
  *out = s_est;
  out->estouros = s_estouros;
  tab5_screen_scroll_cost(&out->px_rolados, &out->us_rolar);
}
