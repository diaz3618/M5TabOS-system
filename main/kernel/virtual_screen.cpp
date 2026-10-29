#include "virtual_screen.hpp"
#include "app.h"

#include "compositor.hpp"
#include "tab5_frame.hpp"
#include "tab5_screen.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"

// DIAGNOSIS (console: screen scroll cpu|dma). true = scrolling shifts by CPU
// within the page of behind, as before of DMA - to compare the two paths
// on the device without re-recording.
bool g_screen_rolar_cpu = false;

static const char *TAG = "tela_virtual";

static const int FB_L = 720;
static const int FB_A = 1280;

// -------------------------------------------------------------------------------
// THE TRASH CAN
// -------------------------------------------------------------------------------
// Design WITHOUT panel (sleep, change of clock): there is no page where to write,
// and the old pointer points to an already freed framebuffer. The pixel has to
// go somewhere - goes here, and the log warns once. Only allocated if
// this actually happens; with the right system, there never is.
static void* s_lixeira = nullptr;

static void* lixeira(void)
{
  if (!s_lixeira) {
    s_lixeira = heap_caps_aligned_calloc(128, 1, (size_t)FB_L * FB_A * 2, MALLOC_CAP_SPIRAM);
    ESP_LOGE(TAG, "desenho com o painel desligado - indo para a lixeira (%s)",
             s_lixeira ? "1,8 MB na PSRAM" : "SEM MEMORIA");
  }
  return s_lixeira;
}

// THE PANEL RETURNED: the trash can exit. It only exists for one drawing out of turn
// with panel disabled (sleep); saved, there were 1.8 MB of PSRAM stuck until
// restart because of one single drawing lost.
static void release_lixeira(void)
{
  if (!s_lixeira) { return; }
  heap_caps_free(s_lixeira);
  s_lixeira = nullptr;
  ESP_LOGI(TAG, "painel de volta - lixeira solta (1,8 MB de PSRAM devolvidos)");
}

// -------------------------------------------------------------------------------
// THE TRACKER PANEL
// -------------------------------------------------------------------------------
// A common Panel_Sprite, which before each writing OPENS the frame and notes the
// rectangle touched. These are ALL the ports through which M5GFX writes to a sprite
// (LGFX_Sprite.hpp: setWindow+writeBlock/writePixels, drawPixel, fillRect,
// writeImage[ARGB], copyRect). Text, line, circle, image - everything goes down
// for one of them.
//
// The coordinates that arrive here are LOGICAL (already rotated by rotation). O
// annotated rectangle has to be PHYSICAL, because that's what the composer copies -
// so the conversion is the SAME as Panel_Sprite does inside
// (LGFX_Sprite.cpp, writeFillRectPreclipped), repeated here.
struct TrackedPanel : public lgfx::Panel_Sprite
{
  // JUST CHANGE THE POINTER. setBuffer() would redo the rotation and reset the window of
  // writing - and the switch happens in the MIDDLE of a write (between the setWindow and
  // o writeBlock, when the first write of iteration opens the frame).
  // SpriteBuffer::reset() with "preallocated" memory does not free anything.
  void apontar(void* buf)
  {
    if (buf != _img.get()) { _img.reset(buf); }
  }

  // Opens the frame (if it isn't already) and ensures that the pixel goes to the
  // page behind it. Called BEFORE all writing.
  void open(void)
  {
    void* tras = comp_abrir();
    apontar(tras ? tras : lixeira());
    if (tras && s_lixeira) { release_lixeira(); }   // already pointing to the page, not to it
  }

  tab5_rect_t fisico(int x, int y, int w, int h) const
  {
    const uint_fast8_t r = _rotation;
    if (r) {
      if ((1u << r) & 0b10010110) { y = _height - (y + h); }
      if (r & 2)                  { x = _width  - (x + w); }
      if (r & 1) { std::swap(x, y); std::swap(w, h); }
    }
    return { (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h };
  }

  // 'opaque': writing covers the entire rectangle with solid color (fillRect) -
  // what the back page had was late doesn't need to come from the front.
  void anotar(int x, int y, int w, int h, bool opaco = false)
  {
    if (w <= 0 || h <= 0) { return; }
    open();
    const tab5_rect_t f = fisico(x, y, w, h);
    // Shortcut: the same rectangle of new (text are tens of fillRect
    // small ones within the same glyph) do not need to go to the list.
    // It is valid only within the SAME round: after the composer has consumed the
    // list, the same rectangle touched of new and new change.
    const uint32_t round = comp_rodada();
    if (round == _ult_round
     && f.x >= _ult_x && f.y >= _ult_y && f.x + f.l <= _ult_x + _ult_l && f.y + f.a <= _ult_y + _ult_a) { return; }
    // The page behind may still be missing what the previous frame changed
    // here (lazy sync): comes of front BEFORE of pixel. The shortcut
    // above can skip this - the rectangle larger than contains has already been prepared.
    if (opaco) { comp_cobrir(f); } else { comp_preparar(f); }
    comp_invalidar(f);
    _ult_x = f.x; _ult_y = f.y; _ult_l = f.l; _ult_a = f.a; _ult_round = round;
  }
  void esquecer_atalho(void) { _ult_l = 0; _ult_a = 0; }

  void setWindow(uint_fast16_t xs, uint_fast16_t ys, uint_fast16_t xe, uint_fast16_t ye) override
  {
    Panel_Sprite::setWindow(xs, ys, xe, ye);
    _jan_marcada = false;
  }
  void writeBlock(uint32_t color, uint32_t len) override
  {
    marcar_window();
    Panel_Sprite::writeBlock(color, len);
  }
  void writePixels(lgfx::pixelcopy_t* p, uint32_t len, bool dma) override
  {
    marcar_window();
    Panel_Sprite::writePixels(p, len, dma);
  }
  void drawPixelPreclipped(uint_fast16_t x, uint_fast16_t y, uint32_t color) override
  {
    anotar(x, y, 1, 1);
    Panel_Sprite::drawPixelPreclipped(x, y, color);
  }
  // LARGE RECTANGULE GOES TO PPA. Cleaning the usable area (1.6 MB) cost
  // 20-25 ms of CPU every time the app is opened; the fill engine
  // writes, without the CPU. Small rectangle (text, borders) stays in CPU: ai a
  // round trip and iteration on PPA costs more than paint.
  static constexpr uint32_t PREENCHER_PPA_MIN = 16384;   // px (e.g. 720x23)

  void writeFillRectPreclipped(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, uint32_t color) override
  {
    anotar(x, y, w, h, true);   // solid color in entire rectangle
    if ((uint32_t)w * h >= PREENCHER_PPA_MIN && _write_bits == 16
     && tab5_frame_fill(_img.get(), fisico(x, y, w, h), (uint16_t)color)) { return; }
    Panel_Sprite::writeFillRectPreclipped(x, y, w, h, color);
  }
  void writeImage(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, lgfx::pixelcopy_t* p, bool dma) override
  {
    anotar(x, y, w, h);
    Panel_Sprite::writeImage(x, y, w, h, p, dma);
  }
  void writeImageARGB(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, lgfx::pixelcopy_t* p) override
  {
    anotar(x, y, w, h);
    Panel_Sprite::writeImageARGB(x, y, w, h, p);
  }

  // ROLLING BY DMA. Moving the content of a window was one memmove of
  // CPU within the same image (~1.6 MB per drag step). Now the page
  // of FRONT - which has exactly what is on screen - and copied, moved,
  // for the back: different buffers, without overlapping, and DMA2D does the
  // work ~1.8x faster than the CPU, which stays free.
  //
  // The condition: the ORIGIN cannot have been drawn in this frame (the front
  // doesn't have this drawing); if so, it falls into copyRect of CPU, which reads the of
  // back. And what the sync had pending within the destination was never
  // copied: DMA writes by top (comp_copiar_da_frente).
  void copyRect(uint_fast16_t dx, uint_fast16_t dy, uint_fast16_t w, uint_fast16_t h, uint_fast16_t sx, uint_fast16_t sy) override
  {
    if (w == 0 || h == 0) { return; }
    const tab5_rect_t dst = fisico(dx, dy, w, h);
    const tab5_rect_t src = fisico(sx, sy, w, h);
    open();
    if (!g_screen_rolar_cpu && !comp_desenhado_neste_quadro(src) && comp_copiar_da_frente(dst, src.x, src.y)) {
      anotar(dx, dy, w, h);
      return;
    }
    comp_preparar(src);          // the CPU will READ the source on the back page
    anotar(dx, dy, w, h);
    Panel_Sprite::copyRect(dx, dy, w, h, sx, sy);
  }

  // READING also needs a back page (smooth text with no background,
  // and who else read pixel of iteration).
  void readRect(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, void* dst, lgfx::pixelcopy_t* p) override
  {
    if (w && h) { comp_preparar(fisico(x, y, w, h)); }
    Panel_Sprite::readRect(x, y, w, h, dst, p);
  }

private:
  // Entire window, once per setWindow - writeBlock/writePixels can come
  // in several calls to the same window.
  void marcar_window(void)
  {
    if (_jan_marcada) { return; }
    anotar(_xs, _ys, _xe - _xs + 1, _ye - _ys + 1);
    _jan_marcada = true;
  }
  bool _jan_marcada = false;
  int  _ult_x = 0, _ult_y = 0, _ult_l = 0, _ult_a = 0;
  uint32_t _ult_round = 0;
};

// The object of drawing: a common LovyanGFX whose panel is the tracker.
class TelaVirtual : public lgfx::LovyanGFX
{
public:
  TrackedPanel panel;

  TelaVirtual(void) { _panel = &panel; }

  // The size and format comes from the page of panel; the pointer itself changes the
  // each frame (TrackedPanel::point).
  bool create(void* pagina)
  {
    // SAME format of framebuffer (Panel_DSI forces rgb565_nonswapped).
    setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    panel.setBuffer(pagina, FB_L, FB_A, &_write_conv);
    setRotation(0);   // also sets screen size cropping
    return true;
  }
};

static TelaVirtual* s_tv = nullptr;

bool virtual_screen_start(void)
{
  if (s_tv) { return true; }
  if (!tab5_screen_init()) {
    ESP_LOGE(TAG, "painel sem paginas - seguindo com desenho direto no M5.Display");
    return false;
  }
  s_tv = new TelaVirtual();
  // It starts pointing to the FRONT: only reading until the first writing, which
  // opens the frame and switches to the back.
  s_tv->create(tab5_screen_page(false));
  // The drawing of emoji and one hook BY OBJECT of screen (setEmojiCallback): o
  // of physical display is not valid here. Without copying, all text drawn by
  // composer lost emojis.
  s_tv->setEmojiCallback(M5.Display.getEmojiCallback());
  tab5_screen_set_target(s_tv);
  ESP_LOGI(TAG, "desenho direto na pagina escondida, sem copia intermediaria");
  return true;
}

lgfx::LovyanGFX& virtual_screen(void)
{
  if (s_tv) { return *s_tv; }
  return M5.Display;
}

void virtual_screen_follow_rotation(void)
{
  if (!s_tv) { return; }
  const uint8_t r = M5.Display.getRotation();
  if (s_tv->getRotation() != r) {
    s_tv->setRotation(r);
    s_tv->panel.esquecer_atalho();
  }
}

void virtual_screen_cover(int x, int y, int w, int h)
{
  if (s_tv) { s_tv->panel.anotar(x, y, w, h, true); }
}

void virtual_screen_invalidate_everything(void)
{
  comp_invalidar_tudo();
}

// -------------------------------------------------------------------------------
// DMA SURFACE (kernel/app.h)
// -------------------------------------------------------------------------------
// An OWN 720x1280 image, allocated in the first request and returned when the
// app closes: the DMA of app (the PPA of camera) writes to it at his own pace, and the
// each draw() the changed area goes to the back page by DMA2D
// (system_surface_changed). Before it was the virtual screen itself - which existed
// always, for everyone; Now only those who use it pay, and only while using it.
static void* s_surface = nullptr;

bool system_surface(const area_t& r, surface_t* sup)
{
  if (!s_tv || !sup || s_tv->getRotation() != 0) { return false; }
  if (r.x < 0 || r.y < 0 || r.width <= 0 || r.height <= 0
   || r.x + r.width > FB_L || r.y + r.height > FB_A) { return false; }
  // FULL WIDTH AND ALIGNED LINES. The PPA of whoever writes INVALIDATES the
  // cache of entire destination lines (ppa_srm.c, "out_ext_window"),
  // rounded to the line of cache of 128 B. A line is 1440 B; 8
  // lines = 11520 B = 90 x 128: with borders in multiples of 8, none line
  // of cache stays divided with one neighbor.
  if (r.x != 0 || r.width != FB_L || (r.y % 8) != 0 || (r.height % 8) != 0) {
    ESP_LOGE(TAG, "superficie %dx%d em (%d,%d) recusada: precisa largura inteira e y/altura multiplos de 8",
             r.width, r.height, r.x, r.y);
    return false;
  }
  if (!s_surface) {
    s_surface = heap_caps_aligned_calloc(128, 1, (size_t)FB_L * FB_A * 2, MALLOC_CAP_SPIRAM);
    if (!s_surface) {
      ESP_LOGE(TAG, "sem PSRAM para a superficie de DMA (1,8 MB)");
      return false;
    }
  }
  sup->buf   = s_surface;
  sup->bytes = (size_t)FB_L * FB_A * 2;
  sup->pic_l = FB_L;
  sup->pic_a = FB_A;
  sup->x     = r.x;
  sup->y     = r.y;
  return true;
}

void system_surface_changed(const area_t& r)
{
  if (!s_surface) { return; }
  const tab5_rect_t f = { (int16_t)r.x, (int16_t)r.y, (int16_t)r.width, (int16_t)r.height };
  comp_copiar_imagem(s_surface, f);
  // The copy opened the frame: the tracker now points to the page of
  // back now, and not just in next writing.
  if (s_tv) { s_tv->panel.open(); }
}

void virtual_screen_release_surface(void)
{
  if (!s_surface) { return; }
  heap_caps_free(s_surface);
  s_surface = nullptr;
  ESP_LOGI(TAG, "tela intermediaria liberada (1,8 MB de PSRAM devolvidos)");
}
