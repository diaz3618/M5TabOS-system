// EMOJI - a small set, drawn as IMAGE, instead of font.
//
// ------------------------------------------------------------------------
// WHY IMAGE, AND WHY THIS IS FROM THE KERNEL AND NOT FROM A JUST APP
// ------------------------------------------------------------------------
// The font of this system is one bitmap ASCII 6x8 - there is no emoji, and it never will
// have as source: the art of one emoji of truth does not fit in a drawing of
// character. The output is not a new draw, it SHOWS the drawing that already
// exists - LovyanGFX already has the right hook for this.
//
// setEmojiCallback() (see LGFXBase.hpp) is automatically called by
// library every time write()/drawString() encounters a Unicode code that
// the current font does not know - and that covers ALL emoji, because the font of this
// system and pure ASCII. By registering ONCE at boot, any app that calls
// d.drawString() with a received message starts showing this emoji
// set without knowing that this file exists - and the same principle of rest
// of kernel: anyone who draws text shouldn't have to worry about this.
//
// ------------------------------------------------------------------------
// THE SET, LIMIT AND LICENSE
// ------------------------------------------------------------------------
// ALL emoji of ONE point of Unicode code - 1357 of them (of the 1391 that
// there is version 17.0; 34 are recent additions that Twemoji has not yet
// drew). Art of Twemoji (twemoji.twitter.com), licensed CC-BY 4.0 -
// mandatory assignment, and that's why she lives here.
//
// "ONE POINT OF CODE" is the limit of truth, and it's worth understanding why.
// The style selector (U+FE0F) DOES NOT count against this - the library itself
// already discards it before any emoji arrives on callback (LGFXBase.cpp:
// "if (uniCode >= 0xFE00 && uniCode < 0xFE10) continue"). What stays of OUTSIDE:
//
// - flags: two "regional indicators" glued together (🇧🇷 = U+1F1E7 U+1F1F7)
// - skin tone: the base emoji + one modifier (👍🏽 = U+1F44D U+1F3FD)
// - ZWJ sequences: several emoji joined by U+200D (👨‍👩‍👧 = person+ZWJ+person+ZWJ+child)
//
// Each of these is SEVERAL Unicode codes forming ONE composite image, and the
// hook of LovyanGFX (setEmojiCallback) fires once PER CODE - there is no
// like this architecture, like this, joining N calls into an image only without
// work extra of truth (join pending codes, decide when to
// sequence closed). Stay for when you need it.
//
// ------------------------------------------------------------------------
// FORMAT
// ------------------------------------------------------------------------
// 24x24 (TAB5_EMOJI_LADO) with PALETTE: each emoji has its own list of colors
// RGB565 (no maximum 256 - no Twemoji exceeds 237, the average is 62) and one
// index of 1 byte per pixel. Index 0 (TAB5_EMOJI_TRANSPARENTE) is always
// the background: "don't draw". Transparency is decided by the INDEX, never by
// color - no mixture of pixels can make a false "transparent".
//
// Why palette and not raw RGB565: 2 bytes per pixel become 1, plus the palette.
// 1357 emoji went from ~1.49MB to ~0.91MB of flash, with the SAME image
// pixel by pixel (the generator only changes the way of saving, not the art).
//
// SCREEN SIZE - see tab5_emoji.cpp. Up to 2x the height of line of the bitmap
// exit in native size (24px); above that, in the largest integer multiple; in
// line LESS than 24px it shrinks to its height, so as not to invade the
// lines of top and bottom.
//
// ------------------------------------------------------------------------
// SEARCH
// ------------------------------------------------------------------------
// TAB5_EMOJI_TABELA is ORDERED by code, and tab5_emoji.cpp searches it for
// BINARY SEARCH - with more than a thousand entries, linear scanning would be
// waste (up to 1357 comparisons per unknown emoji, against no maximum
// 11 in binary search). The generator (generate_emoji.py) already writes the table
// ordered; If one day someone edits this by hand, the order MUST be maintained.
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
  uint32_t        codepoint;   // point of code Unicode, WITHOUT the of style selector (FE0F)
  const uint8_t*  indices;     // TAB5_EMOJI_LADO x TAB5_EMOJI_LADO, 1 byte per pixel
  const uint16_t* paleta;      // RGB565 with bytes swapped; [0] and the bottom
} tab5_emoji_t;

static const uint32_t TAB5_EMOJI_TRANSPARENTE = 0;   // index of fund

extern const int             TAB5_EMOJI_LADO;
extern const tab5_emoji_t    TAB5_EMOJI_TABELA[];   // ORDERED by code - see above
extern const int             TAB5_EMOJI_N;

// Registers the callback in M5.Display. Call ONCE, at boot, after
// M5.begin() (the callback is one method of panel, which only exists after that).
void tab5_emoji_start(void);

// ------------------------------------------------------------------------
// PREPARE TEXT FOR DRAWING - cleaning that avoids "garbage on screen"
// ------------------------------------------------------------------------
// Anyone who sets up a message or one name for d.drawString() should go through
// here before, whenever the text comes from outside (another person, someone else) and can
// contain emoji. Solves TWO problems at once:
//
// 1) SAFE CUTTING. A snprintf("%.60s", txt) cuts by BYTE, not by
// character - and one emoji occupies 4 UTF-8 bytes. Cut in the middle of one of them
// leaves orphaned bytes at the end of string, and the UTF-8 decoder has no
// how to know what to do with them: the result is unpredictable, and that was
// the most likely cause of "trash" observed in conversations.
// Here the cut ALWAYS falls on a character border.
//
// 2) ZWJ CHAINS COLLAPSE TO A SINGLE ICON. A composite emoji (family,
// couple, profession...) and VARIOUS Unicode codes pasted by U+200D (the
// "zero width joiner"). This system only has ONE drawing per code (see the
// limit documented at the top of this file) - untreated, "family"
// becomes 3 or 4 loose icons with no visible relationship between them, side
// aside - a line full of meaningless symbols. Here we keep only the FIRST code of chain and discard the
// ZWJ and the rest - one icon only, not perfect (it's not the composite image of
// true), but CONSISTENT: always one per emoji typed, never one
// random row.
//
// 'out' always ends in NUL. If 'in' is greater than 'out_tam' allows, the
// cut falls on the last character boundary that fits - never in the middle.
void tab5_emoji_prepare(const char* in, char* out, size_t out_size);
