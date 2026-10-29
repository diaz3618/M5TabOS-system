// GLYPH CACHE - smooth text without regenerating each character on every draw.
//
// M5GFX renders a VLW character as follows (lgfx_fonts.cpp, VLWfont::drawChar):
// it reads the character's grayscale bitmap and, for EACH antialiased edge
// pixel, calculates the blend with the background and performs a 1x1
// writeFillRect - plus one call per background row. A 30 px character results
// in ~200 calls, each passing through the composer's tracker. Measurements
// taken per app showed that this accounts for almost all the cost of opening
// an app containing text (Tasks, Energy, Settings, home).
//
// Here, the character is rendered ONCE by M5GFX itself onto a temporary
// canvas - producing the exact same result pixel by pixel - and stored as an
// RGB565 image for that combination of font, foreground color, and background
// color. From the second use forward, the entire character is drawn with a
// SINGLE pushImage.
//
// Only characters that can be reproduced exactly are cached:
// - text with a BACKGROUND (background color different from foreground):
// the entire character block is written, including the background,
// so it does not depend on what was previously underneath;
// - size 1 (fonts at their native size, which is the system default);
// - characters that do not overlap the previous character (M5GFX does not
// repaint the background where the previous character has already drawn
// - in such cases, it falls back to the original rendering path).
// Everything else (transparent text, spaces, characters missing from the font)
// follows the original rendering path, with no behavioral differences.

#pragma once

#include <M5Unified.h>

// An M5GFX VLW font with a cache in front of drawChar. Same type
// (ft_vlw), same loading process (loadFont), same metrics - just renders faster.
struct FastFont : public lgfx::VLWfont
{
  size_t drawChar(lgfx::LGFXBase* gfx, int32_t x, int32_t y, uint16_t c,
                  const lgfx::TextStyle* style, lgfx::FontMetrics* metrics,
                  int32_t& filled_x) const override;
};

typedef struct {
  uint32_t acertos;    // characters drawn from the cached image
  uint32_t faltas;     // characters drawn for the first time (and cached)
  uint32_t recusas;    // characters that followed the original rendering path
  uint32_t rec_espaco;     // ...space (only background: already cheap)
  uint32_t rec_sem_fundo;  // ...transparent text (reads what is underneath)
  uint32_t rec_invade;     // ...the previous character overlaps this one
  uint32_t rec_destino;    // ...canvas that is not 16-bit, or with a palette
  uint32_t rec_escala;     // ...scaled size (setTextSize != 1)
  uint32_t rec_outro;      // ...character outside the font, too large, out of memory
  uint32_t limpezas;   // times the cache filled up and restarted
  uint32_t entries;   // characters currently cached
  uint32_t bytes;      // PSRAM used by the images currently
} glifos_estat_t;

void tab5_glifos_estat(glifos_estat_t* out);
