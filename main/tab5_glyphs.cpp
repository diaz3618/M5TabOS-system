#include "tab5_glyphs.hpp"

#include <string.h>
#include <limits.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "glifos";

// ---------------------------------------------------------------------------
// O CACHE
// ---------------------------------------------------------------------------
// Tabela de espalhamento (enderecamento aberto) + uma arena so para as
// imagens, as duas na PSRAM e criadas no primeiro uso. Encheu (arena ou 3/4
// da tabela): recomeca do zero. Simples de proposito - o conjunto de letras
// em uso e pequeno (poucas fontes, poucas combinacoes de cor), e recomecar
// custa so redesenhar cada letra uma vez.
//
// UMA TAREFA SO: so a interface desenha texto (ver tab5_ui.cpp - as fontes ja
// tem essa regra). Nenhuma trava aqui.
static const uint32_t TAB_N       = 4096;          // potencia de 2
// 512 KB encheu na primeira medida (561 letras, ~900 B cada): a interface usa
// mais combinacoes de fonte e cor do que parecia. 1 MB cabe ~1.100.
static const uint32_t ARENA_BYTES = 1024 * 1024;
static const int      RASCUNHO_L  = 160;           // maior letra que entra
static const int      RASCUNHO_A  = 160;

typedef struct {
  const void* fonte;
  uint32_t    frente, fundo;   // rgb888, como o TextStyle traz
  uint32_t    ofs;             // onde a imagem comeca na arena
  uint16_t    codigo;
  uint16_t    l, a;            // tamanho do bloco
  uint16_t    alt_linha;       // metrics->height quando foi guardada
  int16_t     esq;             // inicio do bloco em relacao ao x da letra
  int16_t     dir;             // fim do fundo (filled_x = x + dir)
  uint16_t    avanco;          // quanto o cursor anda
  bool        usada;
} entrada_t;

static entrada_t* s_tab   = nullptr;
static uint8_t*   s_arena = nullptr;
static uint32_t   s_uso   = 0;
static uint32_t   s_n     = 0;
static bool       s_sem_memoria = false;
static glifos_estat_t s_est = {};

static lgfx::LGFX_Sprite* s_rascunho = nullptr;

static bool preparar(void)
{
  if (s_tab) { return true; }
  if (s_sem_memoria) { return false; }
  s_tab   = (entrada_t*)heap_caps_calloc(TAB_N, sizeof(entrada_t), MALLOC_CAP_SPIRAM);
  s_arena = (uint8_t*)heap_caps_malloc(ARENA_BYTES, MALLOC_CAP_SPIRAM);
  s_rascunho = new lgfx::LGFX_Sprite();
  if (s_rascunho) {
    // O MESMO formato das paginas do painel: a imagem guardada vai para a
    // tela sem conversao nenhuma.
    s_rascunho->setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    s_rascunho->setPsram(true);
  }
  if (!s_tab || !s_arena || !s_rascunho || !s_rascunho->createSprite(RASCUNHO_L, RASCUNHO_A)) {
    ESP_LOGE(TAG, "sem PSRAM para o cache de glifos - texto pelo caminho original");
    heap_caps_free(s_tab);   s_tab = nullptr;
    heap_caps_free(s_arena); s_arena = nullptr;
    delete s_rascunho;       s_rascunho = nullptr;
    s_sem_memoria = true;
    return false;
  }
  ESP_LOGI(TAG, "cache de glifos no ar (%u KB de imagens)", (unsigned)(ARENA_BYTES / 1024));
  return true;
}

static void recomecar(void)
{
  memset(s_tab, 0, TAB_N * sizeof(entrada_t));
  s_uso = 0;
  s_n = 0;
  ++s_est.limpezas;
}

static inline uint32_t espalhar(const void* fonte, uint16_t c, uint32_t frente, uint32_t fundo)
{
  uint32_t h = (uint32_t)(uintptr_t)fonte * 2654435761u;
  h ^= (uint32_t)c * 0x9E3779B1u;
  h ^= frente * 0x85EBCA6Bu;
  h ^= fundo  * 0xC2B2AE35u;
  h ^= h >> 15;
  return h & (TAB_N - 1);
}

static entrada_t* procurar(const void* fonte, uint16_t c, uint32_t frente, uint32_t fundo, bool* nova)
{
  uint32_t i = espalhar(fonte, c, frente, fundo);
  for (uint32_t k = 0; k < TAB_N; ++k, i = (i + 1) & (TAB_N - 1)) {
    entrada_t* e = &s_tab[i];
    if (!e->usada) { *nova = true; return e; }
    if (e->fonte == fonte && e->codigo == c && e->frente == frente && e->fundo == fundo) {
      *nova = false;
      return e;
    }
  }
  return nullptr;
}

void tab5_glifos_estat(glifos_estat_t* out)
{
  if (!out) { return; }
  *out = s_est;
  out->entradas = s_n;
  out->bytes = s_uso;
}

// ---------------------------------------------------------------------------
// O DESENHO
// ---------------------------------------------------------------------------
size_t FonteRapida::drawChar(lgfx::LGFXBase* gfx, int32_t x, int32_t y, uint16_t c,
                             const lgfx::TextStyle* style, lgfx::FontMetrics* metrics,
                             int32_t& filled_x) const
{
  // O que nao da para reproduzir exatamente segue o caminho original.
  // Destino de 16 bits: a imagem guardada e RGB565 - num canvas de 24 bits
  // ela chegaria com menos precisao de cor do que o original desenharia.
  const bool com_fundo = style->back_rgb888 != style->fore_rgb888;
  const bool de_16_bits = ((int)gfx->getColorDepth() & 0xFF) == 16;
  uint16_t g = 0;
  // O MOTIVO de cada recusa e contado: e o que diz qual regra vale mudar.
  uint32_t* motivo = nullptr;
  if      (c == 0x20)                                          { motivo = &s_est.rec_espaco; }
  else if (!com_fundo)                                         { motivo = &s_est.rec_sem_fundo; }
  else if (!de_16_bits || gfx->hasPalette())                   { motivo = &s_est.rec_destino; }
  else if (style->size_x != 1.0f || style->size_y != 1.0f)     { motivo = &s_est.rec_escala; }
  else if (!getUnicodeIndex(c, &g) || !preparar())             { motivo = &s_est.rec_outro; }
  if (motivo) {
    ++s_est.recusas;
    ++*motivo;
    return VLWfont::drawChar(gfx, x, y, c, style, metrics, filled_x);
  }

  bool nova = false;
  bool guardou = false;
  entrada_t* e = procurar(this, c, style->fore_rgb888, style->back_rgb888, &nova);
  if (e && !nova && e->alt_linha != (uint16_t)metrics->height) { e = nullptr; }   // outra metrica: nao arrisca

  if (e && nova) {
    // FALTA: mede a letra pelo cabecalho do arquivo (o mesmo que o
    // VLWfont::drawChar le), calcula o bloco que ele escreveria, e desenha a
    // letra UMA vez num rascunho com o proprio M5GFX.
    uint32_t cab[6] = { 0 };
    _fontData->preRead();
    _fontData->seek(28 + g * 28);
    _fontData->read((uint8_t*)cab, 24);
    _fontData->postRead();
    const int32_t h      = (int32_t)__builtin_bswap32(cab[0]);
    const int32_t w      = (int32_t)__builtin_bswap32(cab[1]);
    const int32_t avanco = (int32_t)__builtin_bswap32(cab[2]);
    const int32_t dY     = (int16_t)__builtin_bswap32(cab[3]);
    const int32_t xoff   = (int8_t)__builtin_bswap32(cab[4]);
    const int32_t yoff   = (int32_t)maxAscent - dY;

    // O bloco: da esquerda da tinta (ou do x, se ela nao recua) ate o maior
    // entre o fim da tinta e o avanco; da linha de cima ate a maior entre a
    // altura da linha e o fim da tinta. TODO pixel nele e escrito pelo
    // original (fundo ou letra) - por isso a imagem o substitui por inteiro.
    const int32_t esq = xoff < 0 ? xoff : 0;
    const int32_t dir = (w + xoff) > avanco ? (w + xoff) : avanco;
    const int32_t l   = dir - esq;
    const int32_t a   = (yoff + h) > metrics->height ? (yoff + h) : metrics->height;
    const uint32_t bytes = (uint32_t)l * a * 2;

    // w == 0 (letra sem tinta que nao e espaco): o original nao pinta as
    // linhas da letra, e o bloco teria buraco. Fica com o original.
    if (w <= 0 || h < 0 || l <= 0 || a <= 0 || yoff < 0 || l > RASCUNHO_L || a > RASCUNHO_A) {
      e = nullptr;
    } else {
      if (s_uso + bytes > ARENA_BYTES || s_n >= TAB_N * 3 / 4) {
        recomecar();
        bool n2 = false;
        e = procurar(this, c, style->fore_rgb888, style->back_rgb888, &n2);
      }
      // No rascunho: x = -esq poe o bloco na coluna 0; y = -y_offset poe a
      // linha de cima na linha 0 (o drawChar soma o y_offset). filled_x no
      // minimo = o fundo comeca no inicio do bloco, como numa letra solta.
      int32_t f = INT32_MIN;
      s_rascunho->setClipRect(0, 0, l, a);
      VLWfont::drawChar(s_rascunho, -esq, -metrics->y_offset, c, style, metrics, f);
      const uint16_t* org = (const uint16_t*)s_rascunho->getBuffer();
      uint8_t* dst = s_arena + s_uso;
      for (int32_t r = 0; r < a; ++r) {
        memcpy(dst + (size_t)r * l * 2, org + (size_t)r * RASCUNHO_L, (size_t)l * 2);
      }
      *e = { };
      e->fonte = this; e->codigo = c;
      e->frente = style->fore_rgb888; e->fundo = style->back_rgb888;
      e->ofs = s_uso; e->l = (uint16_t)l; e->a = (uint16_t)a;
      e->alt_linha = (uint16_t)metrics->height;
      e->esq = (int16_t)esq; e->dir = (int16_t)dir; e->avanco = (uint16_t)avanco;
      e->usada = true;
      s_uso += bytes;
      ++s_n;
      guardou = true;
    }
  }

  // A LETRA DE ANTES INVADE ESTA (o fundo dela passou do x desta): o
  // original nao repinta o fundo ali, so a tinta. A imagem pintaria por cima
  // - entao este caso fica com o original.
  if (!e || filled_x > x + e->esq) {
    ++s_est.recusas;
    ++*(e ? &s_est.rec_invade : &s_est.rec_outro);
    return VLWfont::drawChar(gfx, x, y, c, style, metrics, filled_x);
  }

  if (guardou) { ++s_est.faltas; } else { ++s_est.acertos; }
  gfx->pushImage(x + e->esq, y + metrics->y_offset, e->l, e->a,
                 (const lgfx::rgb565_t*)(s_arena + e->ofs));
  filled_x = x + e->dir;
  return e->avanco;
}
