#include "virtual_screen.hpp"
#include "app.h"

#include "compositor.hpp"
#include "tab5_frame.hpp"
#include "tab5_screen.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"

// DIAGNOSTICO (console: tela rolar cpu|dma). true = a rolagem desloca pela CPU
// dentro da pagina de tras, como antes do DMA - para comparar os dois caminhos
// no aparelho sem regravar.
bool g_tela_rolar_cpu = false;

static const char *TAG = "tela_virtual";

static const int FB_L = 720;
static const int FB_A = 1280;

// ---------------------------------------------------------------------------
// A LIXEIRA
// ---------------------------------------------------------------------------
// Desenho SEM painel (repouso, troca de relogio): nao ha pagina onde escrever,
// e o ponteiro antigo aponta para um framebuffer ja liberado. O pixel tem que
// ir para algum lugar - vai para ca, e o log avisa uma vez. So e alocada se
// isso acontecer de fato; com o sistema certo, nunca existe.
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

// O PAINEL VOLTOU: a lixeira sai. Ela so existe para um desenho fora de hora
// com o painel desligado (repouso); guardada, eram 1,8 MB de PSRAM presos ate
// reiniciar por causa de um unico desenho perdido.
static void soltar_lixeira(void)
{
  if (!s_lixeira) { return; }
  heap_caps_free(s_lixeira);
  s_lixeira = nullptr;
  ESP_LOGI(TAG, "painel de volta - lixeira solta (1,8 MB de PSRAM devolvidos)");
}

// ---------------------------------------------------------------------------
// O PAINEL RASTREADOR
// ---------------------------------------------------------------------------
// Um Panel_Sprite comum, que antes de cada escrita ABRE o quadro e anota o
// retangulo tocado. Sao TODAS as portas por onde o M5GFX escreve num sprite
// (LGFX_Sprite.hpp: setWindow+writeBlock/writePixels, drawPixel, fillRect,
// writeImage[ARGB], copyRect). Texto, linha, circulo, imagem - tudo desce
// para uma delas.
//
// As coordenadas que chegam aqui sao LOGICAS (ja giradas pela rotacao). O
// retangulo anotado tem que ser FISICO, porque e o que o compositor copia -
// entao a conversao e a MESMA que o Panel_Sprite faz por dentro
// (LGFX_Sprite.cpp, writeFillRectPreclipped), repetida aqui.
struct PainelRastreado : public lgfx::Panel_Sprite
{
  // TROCA SO O PONTEIRO. setBuffer() refaria a rotacao e zeraria a janela de
  // escrita - e a troca acontece no MEIO de uma escrita (entre o setWindow e
  // o writeBlock, quando a primeira escrita da volta abre o quadro).
  // SpriteBuffer::reset() com memoria "preallocated" nao libera nada.
  void apontar(void* buf)
  {
    if (buf != _img.get()) { _img.reset(buf); }
  }

  // Abre o quadro (se ainda nao estiver) e garante que o pixel vai para a
  // pagina de tras dele. Chamado ANTES de toda escrita.
  void abrir(void)
  {
    void* tras = comp_abrir();
    apontar(tras ? tras : lixeira());
    if (tras && s_lixeira) { soltar_lixeira(); }   // ja apontando para a pagina, nao para ela
  }

  tab5_ret_t fisico(int x, int y, int w, int h) const
  {
    const uint_fast8_t r = _rotation;
    if (r) {
      if ((1u << r) & 0b10010110) { y = _height - (y + h); }
      if (r & 2)                  { x = _width  - (x + w); }
      if (r & 1) { std::swap(x, y); std::swap(w, h); }
    }
    return { (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h };
  }

  // 'opaco': a escrita cobre o retangulo inteiro com cor solida (fillRect) -
  // o que a pagina de tras tinha de atrasado ali nao precisa vir da frente.
  void anotar(int x, int y, int w, int h, bool opaco = false)
  {
    if (w <= 0 || h <= 0) { return; }
    abrir();
    const tab5_ret_t f = fisico(x, y, w, h);
    // Atalho: o mesmo retangulo de novo (texto sao dezenas de fillRect
    // pequenos dentro de um mesmo glifo) nao precisa ir ate a lista.
    // Vale so dentro da MESMA rodada: depois que o compositor consumiu a
    // lista, o mesmo retangulo tocado de novo e mudanca nova.
    const uint32_t rodada = comp_rodada();
    if (rodada == _ult_rodada
     && f.x >= _ult_x && f.y >= _ult_y && f.x + f.l <= _ult_x + _ult_l && f.y + f.a <= _ult_y + _ult_a) { return; }
    // A pagina de tras ainda pode estar sem o que o quadro anterior mudou
    // aqui (sincronia preguicosa): vem da frente ANTES do pixel. O atalho
    // acima pode pular isto - o retangulo maior que o contem ja foi preparado.
    if (opaco) { comp_cobrir(f); } else { comp_preparar(f); }
    comp_invalidar(f);
    _ult_x = f.x; _ult_y = f.y; _ult_l = f.l; _ult_a = f.a; _ult_rodada = rodada;
  }
  void esquecer_atalho(void) { _ult_l = 0; _ult_a = 0; }

  void setWindow(uint_fast16_t xs, uint_fast16_t ys, uint_fast16_t xe, uint_fast16_t ye) override
  {
    Panel_Sprite::setWindow(xs, ys, xe, ye);
    _jan_marcada = false;
  }
  void writeBlock(uint32_t cor, uint32_t len) override
  {
    marcar_janela();
    Panel_Sprite::writeBlock(cor, len);
  }
  void writePixels(lgfx::pixelcopy_t* p, uint32_t len, bool dma) override
  {
    marcar_janela();
    Panel_Sprite::writePixels(p, len, dma);
  }
  void drawPixelPreclipped(uint_fast16_t x, uint_fast16_t y, uint32_t cor) override
  {
    anotar(x, y, 1, 1);
    Panel_Sprite::drawPixelPreclipped(x, y, cor);
  }
  // RETANGULO GRANDE VAI PARA O PPA. A limpeza da area util (1,6 MB) custava
  // 20-25 ms de CPU em toda abertura de app; o motor de preenchimento so
  // escreve, sem a CPU. Retangulo pequeno (texto, bordas) fica na CPU: ai a
  // ida e volta ao PPA custa mais que pintar.
  static constexpr uint32_t PREENCHER_PPA_MIN = 16384;   // px (ex.: 720x23)

  void writeFillRectPreclipped(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, uint32_t cor) override
  {
    anotar(x, y, w, h, true);   // cor solida no retangulo inteiro
    if ((uint32_t)w * h >= PREENCHER_PPA_MIN && _write_bits == 16
     && tab5_quadro_preencher(_img.get(), fisico(x, y, w, h), (uint16_t)cor)) { return; }
    Panel_Sprite::writeFillRectPreclipped(x, y, w, h, cor);
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

  // A ROLAGEM PELO DMA. Deslocar o conteudo de uma janela era um memmove da
  // CPU dentro da mesma imagem (~1,6 MB por passo de arraste). Agora a pagina
  // da FRENTE - que tem exatamente o que esta na tela - e copiada, deslocada,
  // para a de tras: buffers diferentes, sem sobreposicao, e o DMA2D faz o
  // trabalho ~1,8x mais rapido que a CPU, que fica livre.
  //
  // A condicao: a ORIGEM nao pode ter sido desenhada neste quadro (a frente
  // nao tem esse desenho); se tiver, cai no copyRect da CPU, que le a de
  // tras. E o que a sincronia tinha pendente dentro do destino nunca e
  // copiado: o DMA escreve por cima (comp_copiar_da_frente).
  void copyRect(uint_fast16_t dx, uint_fast16_t dy, uint_fast16_t w, uint_fast16_t h, uint_fast16_t sx, uint_fast16_t sy) override
  {
    if (w == 0 || h == 0) { return; }
    const tab5_ret_t dst = fisico(dx, dy, w, h);
    const tab5_ret_t src = fisico(sx, sy, w, h);
    abrir();
    if (!g_tela_rolar_cpu && !comp_desenhado_neste_quadro(src) && comp_copiar_da_frente(dst, src.x, src.y)) {
      anotar(dx, dy, w, h);
      return;
    }
    comp_preparar(src);          // a CPU vai LER a origem na pagina de tras
    anotar(dx, dy, w, h);
    Panel_Sprite::copyRect(dx, dy, w, h, sx, sy);
  }

  // LEITURA tambem precisa da pagina de tras em dia (texto suave sem fundo,
  // e quem mais ler pixel de volta).
  void readRect(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, void* dst, lgfx::pixelcopy_t* p) override
  {
    if (w && h) { comp_preparar(fisico(x, y, w, h)); }
    Panel_Sprite::readRect(x, y, w, h, dst, p);
  }

private:
  // A janela inteira, uma vez por setWindow - writeBlock/writePixels podem vir
  // em varias chamadas para a mesma janela.
  void marcar_janela(void)
  {
    if (_jan_marcada) { return; }
    anotar(_xs, _ys, _xe - _xs + 1, _ye - _ys + 1);
    _jan_marcada = true;
  }
  bool _jan_marcada = false;
  int  _ult_x = 0, _ult_y = 0, _ult_l = 0, _ult_a = 0;
  uint32_t _ult_rodada = 0;
};

// O objeto de desenho: um LovyanGFX comum cujo painel e o rastreador.
class TelaVirtual : public lgfx::LovyanGFX
{
public:
  PainelRastreado painel;

  TelaVirtual(void) { _panel = &painel; }

  // O tamanho e o formato vem da pagina do painel; o ponteiro em si muda a
  // cada quadro (PainelRastreado::apontar).
  bool criar(void* pagina)
  {
    // O MESMO formato do framebuffer (Panel_DSI forca rgb565_nonswapped).
    setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    painel.setBuffer(pagina, FB_L, FB_A, &_write_conv);
    setRotation(0);   // tambem arma o recorte do tamanho da tela
    return true;
  }
};

static TelaVirtual* s_tv = nullptr;

bool tela_virtual_iniciar(void)
{
  if (s_tv) { return true; }
  if (!tab5_tela_iniciar()) {
    ESP_LOGE(TAG, "painel sem paginas - seguindo com desenho direto no M5.Display");
    return false;
  }
  s_tv = new TelaVirtual();
  // Comeca apontando para a FRENTE: so leitura ate a primeira escrita, que
  // abre o quadro e troca para a de tras.
  s_tv->criar(tab5_tela_pagina(false));
  // O desenho de emoji e um hook POR OBJETO de tela (setEmojiCallback): o
  // do display fisico nao vale aqui. Sem copiar, todo texto desenhado pelo
  // compositor perdia os emojis.
  s_tv->setEmojiCallback(M5.Display.getEmojiCallback());
  tab5_tela_definir_alvo(s_tv);
  ESP_LOGI(TAG, "desenho direto na pagina escondida, sem copia intermediaria");
  return true;
}

lgfx::LovyanGFX& tela_virtual(void)
{
  if (s_tv) { return *s_tv; }
  return M5.Display;
}

void tela_virtual_seguir_rotacao(void)
{
  if (!s_tv) { return; }
  const uint8_t r = M5.Display.getRotation();
  if (s_tv->getRotation() != r) {
    s_tv->setRotation(r);
    s_tv->painel.esquecer_atalho();
  }
}

void tela_virtual_cobrir(int x, int y, int w, int h)
{
  if (s_tv) { s_tv->painel.anotar(x, y, w, h, true); }
}

void tela_virtual_invalidar_tudo(void)
{
  comp_invalidar_tudo();
}

// ---------------------------------------------------------------------------
// SUPERFICIE DE DMA (kernel/app.h)
// ---------------------------------------------------------------------------
// Uma imagem 720x1280 PROPRIA, alocada no primeiro pedido e devolvida quando o
// app fecha: o DMA do app (o PPA da camera) escreve nela no ritmo dele, e a
// cada desenhar() a area mudada vai para a pagina de tras por DMA2D
// (sistema_superficie_mudou). Antes era a propria tela virtual - que existia
// sempre, para todos; agora so quem usa paga, e so enquanto usa.
static void* s_superficie = nullptr;

bool sistema_superficie(const area_t& r, superficie_t* sup)
{
  if (!s_tv || !sup || s_tv->getRotation() != 0) { return false; }
  if (r.x < 0 || r.y < 0 || r.largura <= 0 || r.altura <= 0
   || r.x + r.largura > FB_L || r.y + r.altura > FB_A) { return false; }
  // LARGURA INTEIRA E LINHAS ALINHADAS. O PPA de quem escreve INVALIDA o
  // cache das linhas de destino inteiras (ppa_srm.c, "out_ext_window"),
  // arredondadas para a linha de cache de 128 B. Uma linha tem 1440 B; 8
  // linhas = 11520 B = 90 x 128: com bordas em multiplos de 8, nenhuma linha
  // de cache fica dividida com um vizinho.
  if (r.x != 0 || r.largura != FB_L || (r.y % 8) != 0 || (r.altura % 8) != 0) {
    ESP_LOGE(TAG, "superficie %dx%d em (%d,%d) recusada: precisa largura inteira e y/altura multiplos de 8",
             r.largura, r.altura, r.x, r.y);
    return false;
  }
  if (!s_superficie) {
    s_superficie = heap_caps_aligned_calloc(128, 1, (size_t)FB_L * FB_A * 2, MALLOC_CAP_SPIRAM);
    if (!s_superficie) {
      ESP_LOGE(TAG, "sem PSRAM para a superficie de DMA (1,8 MB)");
      return false;
    }
  }
  sup->buf   = s_superficie;
  sup->bytes = (size_t)FB_L * FB_A * 2;
  sup->pic_l = FB_L;
  sup->pic_a = FB_A;
  sup->x     = r.x;
  sup->y     = r.y;
  return true;
}

void sistema_superficie_mudou(const area_t& r)
{
  if (!s_superficie) { return; }
  const tab5_ret_t f = { (int16_t)r.x, (int16_t)r.y, (int16_t)r.largura, (int16_t)r.altura };
  comp_copiar_imagem(s_superficie, f);
  // A copia abriu o quadro: o rastreador passa a apontar para a pagina de
  // tras ja agora, e nao so na proxima escrita.
  if (s_tv) { s_tv->painel.abrir(); }
}

void tela_virtual_soltar_superficie(void)
{
  if (!s_superficie) { return; }
  heap_caps_free(s_superficie);
  s_superficie = nullptr;
  ESP_LOGI(TAG, "tela intermediaria liberada (1,8 MB de PSRAM devolvidos)");
}
