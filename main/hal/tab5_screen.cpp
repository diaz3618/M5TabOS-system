#include "tab5_screen.hpp"

#include "tab5_internal_dsi_panel.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "tela";

static esp_lcd_panel_handle_t s_handle = nullptr;   // panel com que fomos preparados
static void*     s_fb[2]     = { nullptr, nullptr };
static uint8_t** s_linhas[2] = { nullptr, nullptr };
static uint8_t** s_nosso     = nullptr;   // o vetor que ALOCAMOS (o outro e do M5GFX)
static int       s_largura   = 0;
static int       s_altura    = 0;
static int       s_frente    = 0;         // qual page o DSI esta scanning
static bool      s_pronto    = false;
static uint32_t  s_geracao   = 0;
volatile uint32_t g_tab5_screen_epoch = 0;

static lgfx::LovyanGFX* s_alvo = nullptr;
static inline lgfx::LovyanGFX& alvo(void) { return s_alvo ? *s_alvo : (lgfx::LovyanGFX&)M5.Display; }

void tab5_screen_set_target(lgfx::LovyanGFX* d) { s_alvo = d; }
uint32_t tab5_screen_generation(void) { return s_geracao; }

// ---------------------------------------------------------------------------
// AS PAGINAS
// ---------------------------------------------------------------------------
// O M5GFX so conhece o fb0: o _lines_buffer dele e um vetor de ponteiros de
// linha para ele. Montamos um gemeo para o fb1 e trocamos qual o M5GFX usa a
// cada troca de page - assim o M5.Display aponta sempre para a page
// VISIVEL. Ninguem deveria desenhar nele (o alvo e a tela virtual), mas se
// algo desenhar, cai onde se ve, e nao numa page que o compositor vai
// sobrescrever sem aviso.
void tab5_screen_shutdown(void)
{
  auto* p = tab5_internal_dsi_panel();
  if (p && s_linhas[0]) {
    // SEMPRE devolve o vetor ORIGINAL do M5GFX. O desmontar_tela() do
    // tab5_power libera o que estiver em _lines_buffer; se fosse o NOSSO,
    // liberariamos o mesmo bloco duas vezes (o double free).
    p->set_lines(s_linhas[0]);
  }
  if (s_nosso) { heap_caps_free(s_nosso); s_nosso = nullptr; }
  tab5_frame_shutdown();
  s_linhas[0] = s_linhas[1] = nullptr;
  s_fb[0] = s_fb[1] = nullptr;
  s_handle = nullptr;
  s_frente = 0;
  s_pronto = false;
  ++g_tab5_screen_epoch;
}

bool tab5_screen_init(void)
{
  auto* p = tab5_internal_dsi_panel();
  if (p == nullptr) { return false; }

  esp_lcd_panel_handle_t h = p->get_handle();
  if (h == nullptr) { return false; }

  // Painel igual e ja preparado: nada a fazer. Painel DIFERENTE = o sleep
  // (ou a troca de clock) destruiu e remontou o DSI.
  if (s_pronto && h == s_handle) { return true; }
  if (s_pronto) { tab5_screen_shutdown(); }

  void* fb0 = nullptr;
  void* fb1 = nullptr;
  if (esp_lcd_dpi_panel_get_frame_buffer(h, 2, &fb0, &fb1) != ESP_OK
   || fb0 == nullptr || fb1 == nullptr) {
    ESP_LOGE(TAG, "o panel nao tem duas paginas");
    return false;
  }

  uint8_t** doM5 = p->get_lines();
  if (doM5 == nullptr) { return false; }

  const int height = p->height();
  // Mesma formula do Panel_DSI::init - RGB565, linha alinhada em 4 bytes.
  const size_t step = ((p->width() * 16 >> 3) + 3) & ~3u;

  // Vetor de PONTEIROS de linha: so a CPU le (para achar onde desenhar a
  // linha y); o DMA do DSI le o framebuffer, nunca este vetor. Por isso vai
  // para a PSRAM - sao 5KB (1280 ponteiros) que saem da SRAM interna.
  uint8_t** meu = (uint8_t**)heap_caps_malloc(height * sizeof(uint8_t*), MALLOC_CAP_SPIRAM);
  if (meu == nullptr) {
    ESP_LOGE(TAG, "sem memoria para o vetor de linhas (%d ponteiros)", height);
    return false;
  }
  uint8_t* linha = (uint8_t*)fb1;
  for (int y = 0; y < height; ++y) { meu[y] = linha; linha += step; }

  s_handle    = h;
  s_fb[0]     = fb0;
  s_fb[1]     = fb1;
  s_largura   = p->width();
  s_altura    = height;
  s_linhas[0] = doM5;
  s_linhas[1] = meu;
  s_nosso     = meu;
  s_frente    = 0;
  s_pronto    = true;
  ++s_geracao;
  ++g_tab5_screen_epoch;

  ESP_LOGI(TAG, "tela com page dupla (desenha numa, mostra a outra)");
  tab5_frame_init(h);
  return true;
}

void* tab5_screen_page(bool back)
{
  if (!tab5_screen_init()) { return nullptr; }
  return s_fb[back ? 1 - s_frente : s_frente];
}

bool tab5_screen_request_swap(int y0, int y1)
{
  auto* p = tab5_internal_dsi_panel();
  if (!s_pronto || p == nullptr || y1 <= y0) { return false; }
  const int atras = 1 - s_frente;
  // O ponteiro e o proprio framebuffer: o driver so muda o indice, que vale
  // no proximo end de frame.
  if (esp_lcd_panel_draw_bitmap(s_handle, 0, y0, s_largura, y1, s_fb[atras]) != ESP_OK) { return false; }
  tab5_frame_arm_wait();
  s_frente = atras;
  p->set_lines(s_linhas[s_frente]);
  return true;
}

// ---------------------------------------------------------------------------
// REGIAO SABIDAMENTE LIMPA
// ---------------------------------------------------------------------------
// Numa repintura completa o sistema limpa a area util e logo em seguida o app
// pede a mesma limpeza. A regra de dispensar mora AQUI, e nao em cada app:
// o app pede sempre, sem condicao, e acerta por omissao.
//
// Varias regioes (e nao uma so) porque uma passada limpa areas separadas com
// cores diferentes - area util, barra de baixo, moldura de cima.
#define LIM_MAX 4
static struct { int x, y, l, a; uint16_t color; } s_lim[LIM_MAX];
static int s_n_lim = 0;
static uint32_t s_lim_evitadas = 0;

void tab5_screen_mark_clean(int x, int y, int width, int height, uint16_t color)
{
  if (s_n_lim >= LIM_MAX) { return; }
  s_lim[s_n_lim++] = { x, y, width, height, color };
}

void tab5_screen_forget_clean(void) { s_n_lim = 0; }
uint32_t tab5_screen_avoided_clears(void) { return s_lim_evitadas; }

static bool ja_esta_limpo(int x, int y, int l, int a, uint16_t color)
{
  for (int i = 0; i < s_n_lim; ++i) {
    if (color != s_lim[i].color) { continue; }
    if (x >= s_lim[i].x && y >= s_lim[i].y
     && (x + l) <= (s_lim[i].x + s_lim[i].l)
     && (y + a) <= (s_lim[i].y + s_lim[i].a)) { return true; }
  }
  return false;
}

void tab5_screen_clear(int x, int y, int width, int height, uint16_t color)
{
  if (width <= 0 || height <= 0) { return; }
  if (ja_esta_limpo(x, y, width, height, color)) { ++s_lim_evitadas; return; }
  alvo().fillRect(x, y, width, height, color);
}

// ---------------------------------------------------------------------------
// ROLAGEM - reaproveitar pixel em vez de redesenhar
// ---------------------------------------------------------------------------
// Um copyRect so da area inteira: a tela virtual nao e varrida por ninguem,
// entao nao ha rajada a espacar nem feixe a vencer. O Panel_Sprite copia linha
// a linha na ordem certa para source e target sobrepostos.
static uint64_t s_rol_px = 0;
static uint64_t s_rol_us = 0;

bool tab5_screen_scroll(int x, int y, int width, int height, int delta, uint16_t color)
{
  if (width <= 0 || height <= 0 || delta == 0) { return false; }
  const int ad = (delta < 0) ? -delta : delta;
  if (ad >= height) { return false; }

  auto& d = alvo();
  const int h = height - ad;
  const int64_t t0 = esp_timer_get_time();
  if (delta > 0) {
    d.copyRect(x, y, width, h, x, y + delta);   // conteudo sobe
    d.fillRect(x, y + h, width, ad, color);       // tira nova embaixo
  } else {
    d.copyRect(x, y + ad, width, h, x, y);      // conteudo desce
    d.fillRect(x, y, width, ad, color);           // tira nova em cima
  }
  s_rol_us += (uint64_t)(esp_timer_get_time() - t0);
  s_rol_px += (uint64_t)width * h;
  return true;
}

void tab5_screen_scroll_cost(uint64_t* px, uint64_t* us)
{
  if (px) { *px = s_rol_px; }
  if (us) { *us = s_rol_us; }
}
