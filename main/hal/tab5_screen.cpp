#include "tab5_screen.hpp"

#include "tab5_internal_dsi_panel.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "screen";

static esp_lcd_panel_handle_t s_handle = nullptr;   // panel with which we were prepared
static void*     s_fb[2]     = { nullptr, nullptr };
static uint8_t** s_lines[2] = { nullptr, nullptr };
static uint8_t** s_nosso     = nullptr;   // the vector we ALLOCATED (the other one is M5GFX)
static int       s_width   = 0;
static int       s_height    = 0;
static int       s_front_page    = 0;         // which page is the DSI scanning
static bool      s_ready    = false;
static uint32_t  s_geracao   = 0;
volatile uint32_t g_tab5_screen_epoch = 0;

static lgfx::LovyanGFX* s_draw_target = nullptr;
static inline lgfx::LovyanGFX& draw_target(void) { return s_draw_target ? *s_draw_target : (lgfx::LovyanGFX&)M5.Display; }

void tab5_screen_set_target(lgfx::LovyanGFX* d) { s_draw_target = d; }
uint32_t tab5_screen_generation(void) { return s_geracao; }

// -------------------------------------------------------------------------------
// THE PAGES
// -------------------------------------------------------------------------------
// The M5GFX only knows fb0: its _lines_buffer and one vector of pointers of
// line to him. We set up a twin for the fb1 and swapped which one the M5GFX uses
// each change of page - so the M5.Display always points to the page
// VISIBLE. Nobody should draw on it (the target and the virtual screen), but if
// something draws, it falls where it is seen, and not on a page that the composer goes to
// overwrite without warning.
void tab5_screen_shutdown(void)
{
  auto* p = tab5_internal_dsi_panel();
  if (p && s_lines[0]) {
    // ALWAYS returns the ORIGINAL vector of M5GFX. The disassemble_tela() of
    // tab5_power releases whatever is in _lines_buffer; If it were OUR,
    // we would free the same block twice (double free).
    p->set_lines(s_lines[0]);
  }
  if (s_nosso) { heap_caps_free(s_nosso); s_nosso = nullptr; }
  tab5_frame_shutdown();
  s_lines[0] = s_lines[1] = nullptr;
  s_fb[0] = s_fb[1] = nullptr;
  s_handle = nullptr;
  s_front_page = 0;
  s_ready = false;
  ++g_tab5_screen_epoch;
}

bool tab5_screen_init(void)
{
  auto* p = tab5_internal_dsi_panel();
  if (p == nullptr) { return false; }

  esp_lcd_panel_handle_t h = p->get_handle();
  if (h == nullptr) { return false; }

  // Panel the same and already prepared: nothing to do. DIFFERENT panel = sleep
  // (or changing the clock) destroyed and reassembled the DSI.
  if (s_ready && h == s_handle) { return true; }
  if (s_ready) { tab5_screen_shutdown(); }

  void* fb0 = nullptr;
  void* fb1 = nullptr;
  if (esp_lcd_dpi_panel_get_frame_buffer(h, 2, &fb0, &fb1) != ESP_OK
   || fb0 == nullptr || fb1 == nullptr) {
    ESP_LOGE(TAG, "panel does not have two framebuffers");
    return false;
  }

  uint8_t** doM5 = p->get_lines();
  if (doM5 == nullptr) { return false; }

  const int height = p->height();
  // Same formula of Panel_DSI::init - RGB565, line aligned in 4 bytes.
  const size_t step = ((p->width() * 16 >> 3) + 3) & ~3u;

  // Vector of POINTERS of line: only the CPU reads (to find where to draw the
  // line y); DMA of DSI reads the framebuffer, never this vector. That's why it will
  // for PSRAM - there are 5KB (1280 pointers) that come out of internal SRAM.
  uint8_t** meu = (uint8_t**)heap_caps_malloc(height * sizeof(uint8_t*), MALLOC_CAP_SPIRAM);
  if (meu == nullptr) {
    ESP_LOGE(TAG, "out of memory for the line-pointer array (%d pointers)", height);
    return false;
  }
  uint8_t* line = (uint8_t*)fb1;
  for (int y = 0; y < height; ++y) { meu[y] = line; line += step; }

  s_handle    = h;
  s_fb[0]     = fb0;
  s_fb[1]     = fb1;
  s_width   = p->width();
  s_height    = height;
  s_lines[0] = doM5;
  s_lines[1] = meu;
  s_nosso     = meu;
  s_front_page    = 0;
  s_ready    = true;
  ++s_geracao;
  ++g_tab5_screen_epoch;

  ESP_LOGI(TAG, "double-buffered screen (draw on one page, display the other)");
  tab5_frame_init(h);
  return true;
}

void* tab5_screen_page(bool back)
{
  if (!tab5_screen_init()) { return nullptr; }
  return s_fb[back ? 1 - s_front_page : s_front_page];
}

bool tab5_screen_request_swap(int y0, int y1)
{
  auto* p = tab5_internal_dsi_panel();
  if (!s_ready || p == nullptr || y1 <= y0) { return false; }
  const int atras = 1 - s_front_page;
  // The pointer is the framebuffer itself: the driver only changes the index, which is worth
  // at the next end of frame.
  if (esp_lcd_panel_draw_bitmap(s_handle, 0, y0, s_width, y1, s_fb[atras]) != ESP_OK) { return false; }
  tab5_frame_arm_wait();
  s_front_page = atras;
  p->set_lines(s_lines[s_front_page]);
  return true;
}

// -------------------------------------------------------------------------------
// REGION KNOWN TO BE CLEAN
// -------------------------------------------------------------------------------
// In a complete repainting, the system cleans the useful area and then the app
// asks for the same cleaning. The dismissal rule lives HERE, and not in each app:
// the app always asks, without conditions, and gets it right by default.
//
// Several regions (not just one) because one pass cleans separate areas with
// different colors - usable area, bottom bar, top frame.
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
  draw_target().fillRect(x, y, width, height, color);
}

// -------------------------------------------------------------------------------
// SCROLLING - reuse pixel instead of redraw
// -------------------------------------------------------------------------------
// A copyRect only of the entire area: the virtual screen is not scanned by anyone,
// so there is no blast to space or beam to overcome. Panel_Sprite copies line
// the right line in order for overlapping source and target.
static uint64_t s_rol_px = 0;
static uint64_t s_rol_us = 0;

bool tab5_screen_scroll(int x, int y, int width, int height, int delta, uint16_t color)
{
  if (width <= 0 || height <= 0 || delta == 0) { return false; }
  const int ad = (delta < 0) ? -delta : delta;
  if (ad >= height) { return false; }

  auto& d = draw_target();
  const int h = height - ad;
  const int64_t t0 = esp_timer_get_time();
  if (delta > 0) {
    d.copyRect(x, y, width, h, x, y + delta);   // content goes up
    d.fillRect(x, y + h, width, ad, color);       // new strip below
  } else {
    d.copyRect(x, y + ad, width, h, x, y);      // content goes down
    d.fillRect(x, y, width, ad, color);           // new strip on top
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
