#include "tab5_emoji.hpp"

#include <M5Unified.h>
#include <string.h>
#include <stdlib.h>
#include "tab5_unicode.hpp"
#include "fonts/soft_fonts.hpp"

// BINARY SEARCH, not scanning - see warning in tab5_emoji_dados.cpp. With
// 1357 entries, scanning would cost up to 1357 comparisons per emoji
// unknown; binary search costs a maximum of 11. The table MUST be
// ordered by code for this to work - the generator already guarantees this.
static const tab5_emoji_t* achar_emoji(uint32_t code)
{
  int ini = 0, end = TAB5_EMOJI_N - 1;
  while (ini <= end) {
    const int middle = ini + (end - ini) / 2;
    const uint32_t c = TAB5_EMOJI_TABELA[middle].codepoint;
    if (c == code) { return &TAB5_EMOJI_TABELA[middle]; }
    if (c < code)  { ini = middle + 1; } else { end = middle - 1; }
  }
  return nullptr;
}

// THE CALLBACK THAT LOVYANGFX ALREADY KNOWS HOW TO CALL.
//
// Signature fixed by the library (LGFXBase.hpp: emoji_draw_cb_t) - not of
// to change a parameter without breaking the register. 'height' comes in JA pixels
// scaled by the current size of text (see the calculation in
// LGFXBase.cpp: "metrics.height * sy >> 16") - and therefore this callback does not
// need to know what font size was in use.
//
// Returning <=0 says "I don't know this code": the library drops by itself to
// the behavior of always (codepoint 0, which does not draw anything). That's why
// table only needs to know what it recognizes - the rest remains invisible in
// instead of becoming garbage on screen, exactly as it was before this file existed.
static int32_t emoji_cb(lgfx::LGFXBase* gfx, int32_t x, int32_t y, uint32_t code, int32_t height)
{
  const tab5_emoji_t* e = achar_emoji(code);
  if (!e) { return 0; }

  // SCALED TO CELL, not drawn at native size.
  //
  // This used pure pushImage(), in the FIXED size of bitmap
  // (TAB5_EMOJI_LADO, 24px) - and has never been tested against the most common case:
  // text of message uses size 2, and the cell that the library reserves for
  // one emoji there measures 16px of side, not 24. The emoji comes out 8px BIGGER than the
  // cell on each axis, leaking through neighboring top of text.
  //
  // pushImageRotateZoom() solves this by scaling the bitmap of 24x24 to
  // ANY 'height' requested, always centered - the same bitmap works for
  // the small text of a message (16px) and for one eventual size 4 in
  // elsewhere (32px), without needing to save more than one resolution.
  //
  // NO SMOOTHING (the "WithAA" version was taken). The reason:
  // antialiasing MIXES the color of neighboring pixels in edge of drawing - and
  // near the edge of any emoji, one of the neighbors is usually the bottom
  // transparent (in time, the magenta sentinel color). The mixture produced
  // a color that WAS NO LONGER the sentinel, and became a pixel of truth, in
  // wrong color - was seen live: one magenta dot released on black background.
  // Today the background is the INDEX 0 of palette, and with palette the library doesn't even have
  // how to mix - but without AA it is still right: each pixel of origin
  // either It is deep (jumps) or it is not (draws), with no in-between.
  //
  // GEOMETRY: (src_x,src_y) in bitmap maps to (dst_x,dst_y) in screen.
  //
  // THE ACCOUNT THAT IS CANCELED FOR LETTER AND NOT FOR EMOJI - found in
  // tracing the source of library (LGFXBase::write() +
  // GFXfont::drawChar(), both in lgfx_fonts.cpp/LGFXBase.cpp), after
  // one real test ("📡 Test Ok.") showed the antenna clearly lower
  // than the text of SAME bubble, in SAME line.
  //
  // write() shifts the ENTIRE 'y' of line by "-(metrics.y_offset)*sy" BEFORE
  // of loop of characters (y_offset is the only FONT value, not of glyph -
  // GFXfont::updateFontMetric() never writes to it; just getDefaultMetric()
  // writes, once, when setFont() runs). Each NORMAL character passes through
  // GFXfont::drawChar(), which applies "y += metrics.y_offset*sy" of new -
  // CANCELING the offset of line before drawing the glyph. THE EMOJI,
  // because it doesn't go through drawChar(), it receives the 'y' of line WITHOUT cancellation -
  // and stays offset to bottom by exactly "-y_offset * sy" pixels.
  //
  // TO FREESANS12PT7B (the source of body - see
  // TAB5_FONTE_BODY in tab5_ui.hpp): scan the table of glyphs (grep+awk on
  // FreeSans12pt7b.h looking for the smallest yOffset, SAME METHOD validated against
  // DejaVu24 before switch - gave -18, matching the "18" already documented
  // here before) and arrived at glyph_ab=17 (the HIGHEST ascendant of the entire
  // font) - y_offset = -17, height (yAdvance) = 29 = TAB5_FONT_BODY_ALT_PX.
  //
  // Correct and undo this offset (subtract 17/29 of 'height' from 'y'
  // received) and only THEN center the emoji in the body of font itself (the
  // half of top of height, where the letters actually draw) - both
  // accounts become ONE fraction, applied in the center.
  //
  // THIS NUMBER IS SPECIFIC TO FREESANS12PT7B - if TAB5_FONTE_BODY switch of
  // new, recalculate glyph_ab/height of new font (same method: smaller yOffset
  // of table of glyphs) and exchange the account below together.
  // the fraction and LIDA of source in use (the of body became the
  // Roboto smoothed, at various heights) - the fixed 17/29 was only of FreeSans12.
  lgfx::FontMetrics fm;
  gfx->getFont()->getDefaultMetric(&fm);
  const float Y_OFFSET_FRACAO = (fm.height > 0) ? (float)(-fm.y_offset) / (float)fm.height : 17.0f / 29.0f;
  const float FRACAO_VERTICAL = 0.5f - Y_OFFSET_FRACAO; // = -0.22: center of emoji, fixed and centered
  // FULL SCALE: the bitmap is 24 px, and enlarge by fraction (24 -> 29 px)
  // duplicates some lines of pixels and others not - the same aliasing as the
  // sources had. Up to 2x the height of text it exits in its native size,
  // centered in the space of character; above that, in the largest integer multiple.
  //
  // BELOW 24 px it SHRINKS to the height of line: smooth fonts will
  // up to 12 px (generate_fonts.py), and there one emoji of 24 px invaded the line of
  // top and a of bottom. Shrink skips lines of pixels instead of doubling - a
  // loss of detail in a drawing this size is not noticeable; the invasion, yes.
  const float lado = (float)TAB5_EMOJI_LADO;
  const int   mult = (height >= 2 * TAB5_EMOJI_LADO) ? height / TAB5_EMOJI_LADO : 1;
  const float zoom = (height < TAB5_EMOJI_LADO) ? (float)height / lado : (float)mult;
  gfx->pushImageRotateZoom(
      (float)x + (float)height / 2.0f, (float)y + (float)height * FRACAO_VERTICAL,
      lado / 2.0f, lado / 2.0f,
      0.0f,                 // no rotation
      zoom, zoom,
      TAB5_EMOJI_LADO, TAB5_EMOJI_LADO,
      e->indices, TAB5_EMOJI_TRANSPARENTE, lgfx::color_depth_t::palette_8bit,
      // swap565_t: the same reading (bytes swapped) that the library did of
      // uint16_t raw before - the palette was written like this by the generator.
      reinterpret_cast<const lgfx::swap565_t*>(e->paleta));

  // Advances the cursor over the entire CELL (height), not just the bitmap - and the
  // same spacing that a normal character would leave, and what makes the
  // next character does not paste into the emoji.
  return height;
}

void tab5_emoji_start(void)
{
  M5.Display.setEmojiCallback(emoji_cb);
}

// WHAT HAS THE DRAWING: the font (read from VLW itself - the list of glyphs and the
// that generate_fonts.py recorded, without copying to keep in sync) or the emoji.
// The of body and the bold come from the same CHARACTERS; just one.
static bool fonte_tem(uint32_t code)
{
  static uint32_t* s_cods = nullptr;
  static int       s_n = -1;
  if (s_n < 0) {
    s_n = 0;
    if (FONTES_CORPO_N > 0 && FONTES_CORPO[0].tamanho >= 24) {
      const uint8_t* v = FONTES_CORPO[0].data;
      auto be32 = [](const uint8_t* b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; };
      const uint32_t n = be32(v);
      if (24 + n * 28 <= FONTES_CORPO[0].tamanho) {
        s_cods = (uint32_t*)malloc(n * sizeof(uint32_t));
        if (s_cods) { for (uint32_t i = 0; i < n; ++i) { s_cods[i] = be32(v + 24 + i * 28); } s_n = (int)n; }
      }
    }
  }
  for (int i = 0; i < s_n; ++i) { if (s_cods[i] == code) { return true; } }
  return false;
}

static bool desenhavel(uint32_t code)
{
  return fonte_tem(code) || achar_emoji(code) != nullptr;
}

void tab5_emoji_prepare(const char* in, char* out, size_t out_size)
{
  // All cleaning (ZWJ, cutting, and what doesn't have a drawing) lives in
  // tab5_unicode - pure, tested on Mac (tests/rodar_unicode.sh).
  tab5_unicode_prepare(in, out, out_size, desenhavel);
}
