#include "tab5_glyphs.hpp"

#include <string.h>
#include <limits.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "glyphs";

// -------------------------------------------------------------------------------
// THE CACHE
// -------------------------------------------------------------------------------
// Hash table (open addressing) + a dedicated arena for the images,
// both stored in PSRAM and initialized on first use. When full (either the
// arena or 3/4 of the table): reset everything and start over. Intentionally
// simple - the set of characters in use is small (few fonts, few color
// combinations), and resetting only costs rendering each character once again.
//
// SINGLE TASK ONLY: only the UI renders text (see tab5_ui.cpp - the fonts
// already follow this rule). No locking here.
static const uint32_t TAB_N = 4096; // power of 2
// 512 KB filled up in the first measurement (561 characters, ~900 B each): the interface uses
// more combinations of font and color than expected. 1MB fits ~1,100.
static const uint32_t ARENA_BYTES = 1024 * 1024;
static const int SCRATCH_W = 160; // largest letter that enters
static const int SCRATCH_H = 160;

typedef struct
{
  const void *font;
  uint32_t fg, bg; // rgb888, as provided by TextStyle
  uint32_t ofs;    // where the image begins in the arena
  uint16_t code;
  uint16_t w, h;     // block dimensions
  uint16_t line_h;   // metrics->height when cached
  int16_t left;      // start of block relative to the character's x position
  int16_t right;     // end of background (filled_x = x + right)
  uint16_t advance;  // how to make the cursor advance
  bool used;
} entry_t;

static entry_t *s_table = nullptr;
static uint8_t *s_arena = nullptr;
static uint32_t s_used = 0;
static uint32_t s_n = 0;
static bool s_out_of_memory = false;
static glifos_estat_t s_est = {};

static lgfx::LGFX_Sprite *s_scratch = nullptr;

static bool prepare(void)
{
  if (s_table)
  {
    return true;
  }
  if (s_out_of_memory)
  {
    return false;
  }
  s_table = (entry_t *)heap_caps_calloc(TAB_N, sizeof(entry_t), MALLOC_CAP_SPIRAM);
  s_arena = (uint8_t *)heap_caps_malloc(ARENA_BYTES, MALLOC_CAP_SPIRAM);
  s_scratch = new lgfx::LGFX_Sprite();
  if (s_scratch)
  {
    // SAME format as the panel's pages: the saved image goes to the
    // screen without any conversion.
    s_scratch->setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    s_scratch->setPsram(true);
  }
  if (!s_table || !s_arena || !s_scratch || !s_scratch->createSprite(SCRATCH_W, SCRATCH_H))
  {
    ESP_LOGE(TAG, "no PSRAM for the glyph cache - falling back to the original text path");
    heap_caps_free(s_table);
    s_table = nullptr;
    heap_caps_free(s_arena);
    s_arena = nullptr;
    delete s_scratch;
    s_scratch = nullptr;
    s_out_of_memory = true;
    return false;
  }
  ESP_LOGI(TAG, "glyph cache online (%u KB of images)", (unsigned)(ARENA_BYTES / 1024));
  return true;
}

static void restart(void)
{
  memset(s_table, 0, TAB_N * sizeof(entry_t));
  s_used = 0;
  s_n = 0;
  ++s_est.limpezas;
}

static inline uint32_t hash(const void *font, uint16_t c, uint32_t fg, uint32_t bg)
{
  uint32_t h = (uint32_t)(uintptr_t)font * 2654435761u;
  h ^= (uint32_t)c * 0x9E3779B1u;
  h ^= fg * 0x85EBCA6Bu;
  h ^= bg * 0xC2B2AE35u;
  h ^= h >> 15;
  return h & (TAB_N - 1);
}

static entry_t *find(const void *font, uint16_t c, uint32_t fg, uint32_t bg, bool *is_new)
{
  uint32_t i = hash(font, c, fg, bg);
  for (uint32_t k = 0; k < TAB_N; ++k, i = (i + 1) & (TAB_N - 1))
  {
    entry_t *e = &s_table[i];
    if (!e->used)
    {
      *is_new = true;
      return e;
    }
    if (e->font == font && e->code == c && e->fg == fg && e->bg == bg)
    {
      *is_new = false;
      return e;
    }
  }
  return nullptr;
}

void tab5_glifos_estat(glifos_estat_t *out)
{
  if (!out)
  {
    return;
  }
  *out = s_est;
  out->entries = s_n;
  out->bytes = s_used;
}

// -------------------------------------------------------------------------------
// THE DESIGN
// -------------------------------------------------------------------------------
size_t FastFont::drawChar(lgfx::LGFXBase *gfx, int32_t x, int32_t y, uint16_t c,
                             const lgfx::TextStyle *style, lgfx::FontMetrics *metrics,
                             int32_t &filled_x) const
{
  // What cannot be reproduced exactly follows the original path.
  // 16-bit destination: the saved image is RGB565 - on a 24-bit canvas
  // it would arrive with less color precision than the original would draw.
  const bool has_background = style->back_rgb888 != style->fore_rgb888;
  const bool is_16bit = ((int)gfx->getColorDepth() & 0xFF) == 16;
  uint16_t g = 0;
  // The REASON for each refusal is tracked: it says which rule is worth changing.
  uint32_t *reason = nullptr;
  if (c == 0x20)
  {
    reason = &s_est.rec_espaco;
  }
  else if (!has_background)
  {
    reason = &s_est.rec_sem_fundo;
  }
  else if (!is_16bit || gfx->hasPalette())
  {
    reason = &s_est.rec_destino;
  }
  else if (style->size_x != 1.0f || style->size_y != 1.0f)
  {
    reason = &s_est.rec_escala;
  }
  else if (!getUnicodeIndex(c, &g) || !prepare())
  {
    reason = &s_est.rec_outro;
  }
  if (reason)
  {
    ++s_est.recusas;
    ++*reason;
    return VLWfont::drawChar(gfx, x, y, c, style, metrics, filled_x);
  }

  bool is_new = false;
  bool stored = false;
  entry_t *e = find(this, c, style->fore_rgb888, style->back_rgb888, &is_new);
  if (e && !is_new && e->line_h != (uint16_t)metrics->height)
  {
    e = nullptr;
  } // another metric: no risk

  if (e && is_new)
  {
    // MISSING: measures the letter from the file header (the same as
    // VLWfont::drawChar reads), computes the block it would write, and draws the
    // letter ONCE onto a scratch canvas with M5GFX itself.
    uint32_t hdr[6] = {0};
    _fontData->preRead();
    _fontData->seek(28 + g * 28);
    _fontData->read((uint8_t *)hdr, 24);
    _fontData->postRead();
    const int32_t h = (int32_t)__builtin_bswap32(hdr[0]);
    const int32_t w = (int32_t)__builtin_bswap32(hdr[1]);
    const int32_t advance = (int32_t)__builtin_bswap32(hdr[2]);
    const int32_t dY = (int16_t)__builtin_bswap32(hdr[3]);
    const int32_t xoff = (int8_t)__builtin_bswap32(hdr[4]);
    const int32_t yoff = (int32_t)maxAscent - dY;

    // The block: from the left edge of the foreground pixels (or from x, if they
    // do not extend leftward) to the greater of the foreground's right edge and
    // the advance width; from the top row to the greater of the line height and
    // the bottom edge of the foreground pixels. EVERY pixel within this block is
    // written by the original renderer (background or character) - which is why
    // the cached image can replace it entirely.
    const int32_t left = xoff < 0 ? xoff : 0;
    const int32_t right = (w + xoff) > advance ? (w + xoff) : advance;
    const int32_t bw = right - left;
    const int32_t bh = (yoff + h) > metrics->height ? (yoff + h) : metrics->height;
    const uint32_t bytes = (uint32_t)bw * bh * 2;

    // w == 0 (character with no visible pixels that is not a space): the original
    // renderer does not draw the character's rows, and the cached block would
    // contain a gap. Fall back to the original rendering path.
    if (w <= 0 || h < 0 || bw <= 0 || bh <= 0 || yoff < 0 || bw > SCRATCH_W || bh > SCRATCH_H)
    {
      e = nullptr;
    }
    else
    {
      if (s_used + bytes > ARENA_BYTES || s_n >= TAB_N * 3 / 4)
      {
        restart();
        bool is_new2 = false;
        e = find(this, c, style->fore_rgb888, style->back_rgb888, &is_new2);
      }
      // On the scratch canvas: x = -left places the block at column 0; y = -y_offset
      // places the top row at row 0 (drawChar adds y_offset). Set filled_x to the
      // minimum so the background starts at the beginning of the block, as it does
      // for an isolated character.
      int32_t f = INT32_MIN;
      s_scratch->setClipRect(0, 0, bw, bh);
      VLWfont::drawChar(s_scratch, -left, -metrics->y_offset, c, style, metrics, f);
      const uint16_t *src = (const uint16_t *)s_scratch->getBuffer();
      uint8_t *dst = s_arena + s_used;
      for (int32_t row = 0; row < bh; ++row)
      {
        memcpy(dst + (size_t)row * bw * 2, src + (size_t)row * SCRATCH_W, (size_t)bw * 2);
      }
      *e = {};
      e->font = this;
      e->code = c;
      e->fg = style->fore_rgb888;
      e->bg = style->back_rgb888;
      e->ofs = s_used;
      e->w = (uint16_t)bw;
      e->h = (uint16_t)bh;
      e->line_h = (uint16_t)metrics->height;
      e->left = (int16_t)left;
      e->right = (int16_t)right;
      e->advance = (uint16_t)advance;
      e->used = true;
      s_used += bytes;
      ++s_n;
      stored = true;
    }
  }

  // THE PREVIOUS CHARACTER OVERLAPS THIS ONE (its background extended past
  // this character's x position): the original renderer does not repaint the
  // background there, only the foreground pixels. The cached image would paint
  // over it - so this case continues using the original rendering path.
  if (!e || filled_x > x + e->left)
  {
    ++s_est.recusas;
    ++*(e ? &s_est.rec_invade : &s_est.rec_outro);
    return VLWfont::drawChar(gfx, x, y, c, style, metrics, filled_x);
  }

  if (stored)
  {
    ++s_est.faltas;
  }
  else
  {
    ++s_est.acertos;
  }
  gfx->pushImage(x + e->left, y + metrics->y_offset, e->w, e->h,
                 (const lgfx::rgb565_t *)(s_arena + e->ofs));
  filled_x = x + e->right;
  return e->advance;
}
