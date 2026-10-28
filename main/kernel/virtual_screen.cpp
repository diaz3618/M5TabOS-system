#include "virtual_screen.hpp"
#include "app.h"

#include "compositor.hpp"
#include "tab5_frame.hpp"
#include "tab5_screen.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"

// DIAGNOSTICO (console: screen rolar cpu|dma). true = a rolagem desloca pela CPU
// dentro of pagina of tras, como antes of DMA - para comparar os dois caminhos
// no aparelho sem regravar.
bool g_screen_rolar_cpu = false;

static const char *TAG = "tela_virtual";

static const int FB_L = 720;
static const int FB_A = 1280;

// ---------------------------------------------------------------------------
// A LIXEIRA
// ---------------------------------------------------------------------------
// Desenho SEM panel (sleep, troca of clock): nao ha pagina onde write,
// e o ponteiro antigo aponta para one framebuffer ja liberado. O pixel tem que
// ir para algum lugar - vai para ca, e o log avisa uma vez. So e alocada se
// isso acontecer of fato; com o sistema certo, nunca existe.
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

// O PAINEL VOLTOU: a lixeira exit. Ela so existe para one drawing fora of hora
// com o panel disabled (sleep); guardada, eram 1,8 MB of PSRAM presos ate
// reiniciar por causa of one unico drawing perdido.
static void release_lixeira(void)
{
  if (!s_lixeira) { return; }
  heap_caps_free(s_lixeira);
  s_lixeira = nullptr;
  ESP_LOGI(TAG, "painel de volta - lixeira solta (1,8 MB de PSRAM devolvidos)");
}

// ---------------------------------------------------------------------------
// O PAINEL RASTREADOR
// ---------------------------------------------------------------------------
// Um Panel_Sprite comum, que antes of cada escrita ABRE o frame e anota o
// rectangle touched. Sao TODAS as portas por onde o M5GFX escreve num sprite
// (LGFX_Sprite.hpp: setWindow+writeBlock/writePixels, drawPixel, fillRect,
// writeImage[ARGB], copyRect). Texto, line, circulo, imagem - everything desce
// para uma delas.
//
// As coordenadas que chegam aqui sao LOGICAS (ja giradas pela rotation). O
// rectangle anotado tem que ser FISICO, porque e o que o compositor copia -
// entao a conversao e a MESMA que o Panel_Sprite faz por dentro
// (LGFX_Sprite.cpp, writeFillRectPreclipped), repetida aqui.
struct TrackedPanel : public lgfx::Panel_Sprite
{
  // TROCA SO O PONTEIRO. setBuffer() refaria a rotation e zeraria a window of
  // escrita - e a troca acontece no MEIO of uma escrita (entre o setWindow e
  // o writeBlock, when a primeira escrita of iteration abre o frame).
  // SpriteBuffer::reset() com memoria "preallocated" nao libera nada.
  void apontar(void* buf)
  {
    if (buf != _img.get()) { _img.reset(buf); }
  }

  // Abre o frame (se ainda nao estiver) e garante que o pixel vai para a
  // pagina of tras dele. Chamado ANTES of toda escrita.
  void open(void)
  {
    void* tras = comp_abrir();
    apontar(tras ? tras : lixeira());
    if (tras && s_lixeira) { release_lixeira(); }   // ja apontando para a pagina, nao para ela
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

  // 'opaco': a escrita cobre o rectangle inteiro com color solida (fillRect) -
  // o que a pagina of tras tinha of atrasado ali nao precisa vir of frente.
  void anotar(int x, int y, int w, int h, bool opaco = false)
  {
    if (w <= 0 || h <= 0) { return; }
    open();
    const tab5_rect_t f = fisico(x, y, w, h);
    // Atalho: o mesmo rectangle of novo (text sao dezenas of fillRect
    // pequenos dentro of one mesmo glifo) nao precisa ir ate a lista.
    // Vale so dentro of MESMA round: depois que o compositor consumiu a
    // lista, o mesmo rectangle touched of novo e mudanca nova.
    const uint32_t round = comp_rodada();
    if (round == _ult_round
     && f.x >= _ult_x && f.y >= _ult_y && f.x + f.l <= _ult_x + _ult_l && f.y + f.a <= _ult_y + _ult_a) { return; }
    // A pagina of tras ainda pode estar sem o que o frame anterior changed
    // aqui (sincronia preguicosa): vem of frente ANTES of pixel. O atalho
    // acima pode pular isto - o rectangle maior que o contains ja foi preparado.
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
  // RETANGULO GRANDE VAI PARA O PPA. A limpeza of area util (1,6 MB) custava
  // 20-25 ms of CPU em toda abertura of app; o motor of preenchimento so
  // escreve, sem a CPU. Retangulo pequeno (text, bordas) stays in CPU: ai a
  // ida e iteration on PPA custa mais que paint.
  static constexpr uint32_t PREENCHER_PPA_MIN = 16384;   // px (ex.: 720x23)

  void writeFillRectPreclipped(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, uint32_t color) override
  {
    anotar(x, y, w, h, true);   // color solida no rectangle inteiro
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

  // A ROLAGEM PELO DMA. Deslocar o content of uma window era one memmove of
  // CPU dentro of mesma imagem (~1,6 MB por passo of arraste). Agora a pagina
  // of FRENTE - que tem exatamente o que esta in screen - e copiada, deslocada,
  // para a of tras: buffers diferentes, sem sobreposicao, e o DMA2D faz o
  // work ~1,8x mais rapido que a CPU, que stays livre.
  //
  // A condicao: a ORIGEM nao pode ter sido desenhada neste frame (a frente
  // nao tem esse drawing); se tiver, cai no copyRect of CPU, que le a of
  // tras. E o que a sincronia tinha pending dentro of destino nunca e
  // copiado: o DMA escreve por top (comp_copiar_da_frente).
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
    comp_preparar(src);          // a CPU vai LER a origem in pagina of tras
    anotar(dx, dy, w, h);
    Panel_Sprite::copyRect(dx, dy, w, h, sx, sy);
  }

  // LEITURA tambem precisa of pagina of tras em dia (text suave sem fundo,
  // e quem mais read pixel of iteration).
  void readRect(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, void* dst, lgfx::pixelcopy_t* p) override
  {
    if (w && h) { comp_preparar(fisico(x, y, w, h)); }
    Panel_Sprite::readRect(x, y, w, h, dst, p);
  }

private:
  // A window inteira, uma vez por setWindow - writeBlock/writePixels podem vir
  // em varias chamadas para a mesma window.
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

// O objeto of drawing: one LovyanGFX comum cujo panel e o rastreador.
class TelaVirtual : public lgfx::LovyanGFX
{
public:
  TrackedPanel panel;

  TelaVirtual(void) { _panel = &panel; }

  // O tamanho e o formato vem of pagina of panel; o ponteiro em si muda a
  // cada frame (TrackedPanel::apontar).
  bool create(void* pagina)
  {
    // O MESMO formato of framebuffer (Panel_DSI forca rgb565_nonswapped).
    setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    panel.setBuffer(pagina, FB_L, FB_A, &_write_conv);
    setRotation(0);   // tambem arma o recorte of tamanho of screen
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
  // Comeca apontando para a FRENTE: so leitura ate a primeira escrita, que
  // abre o frame e troca para a of tras.
  s_tv->create(tab5_screen_page(false));
  // O drawing of emoji e one hook POR OBJETO of screen (setEmojiCallback): o
  // of display fisico nao vale aqui. Sem copiar, todo text drawn pelo
  // compositor perdia os emojis.
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

// ---------------------------------------------------------------------------
// SUPERFICIE DE DMA (kernel/app.h)
// ---------------------------------------------------------------------------
// Uma imagem 720x1280 PROPRIA, alocada no first request e devolvida when o
// app fecha: o DMA of app (o PPA of camera) escreve nela no ritmo dele, e a
// cada draw() a area mudada vai para a pagina of tras por DMA2D
// (sistema_superficie_mudou). Antes era a propria screen virtual - que existia
// sempre, para todos; now so quem usa paga, e so enquanto usa.
static void* s_surface = nullptr;

bool system_surface(const area_t& r, surface_t* sup)
{
  if (!s_tv || !sup || s_tv->getRotation() != 0) { return false; }
  if (r.x < 0 || r.y < 0 || r.width <= 0 || r.height <= 0
   || r.x + r.width > FB_L || r.y + r.height > FB_A) { return false; }
  // LARGURA INTEIRA E LINHAS ALINHADAS. O PPA of quem escreve INVALIDA o
  // cache das lines of destino inteiras (ppa_srm.c, "out_ext_window"),
  // arredondadas para a line of cache of 128 B. Uma line tem 1440 B; 8
  // lines = 11520 B = 90 x 128: com bordas em multiplos of 8, none line
  // of cache stays dividida com one vizinho.
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
  // A copia abriu o frame: o rastreador passa a apontar para a pagina of
  // tras ja now, e nao so in next escrita.
  if (s_tv) { s_tv->panel.open(); }
}

void virtual_screen_release_surface(void)
{
  if (!s_surface) { return; }
  heap_caps_free(s_surface);
  s_surface = nullptr;
  ESP_LOGI(TAG, "tela intermediaria liberada (1,8 MB de PSRAM devolvidos)");
}
