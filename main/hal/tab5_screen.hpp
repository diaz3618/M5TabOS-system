// THE TWO PAGES OF THE PANEL, and the primitives of drawing in area.
//
// The MIPI-DSI panel of Tab5 scans one framebuffer in PSRAM without stopping. O
// driver of IDF already allocates TWO (num_fbs = 2 in Panel_DSI of M5GFX) and exchanges
// page just changing one index (esp_lcd_panel_dpi.c:506-524) - the change is valid in
// next end of frame.
//
// Whoever writes on the pages is the composer (kernel/compositor.cpp). All
// rest of system draws in virtual screen (kernel/virtual_screen.cpp), which the
// panel never reads. See docs/COMPOSITOR.md.
//
// HISTORY, for those who might find the size strange: this module had
// ~800 lines of defense against write in page that the panel scans - tracks,
// breathers, double buffer only for complete repainting, scroll running against
// the beam. The underlying cause was the bandwidth of PSRAM by half (line of cache
// L2 of 64B instead of 128B) added to visible draw in page. The two
// are finished: the composer draws behind, and the line of cache is 128B.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <M5Unified.h>
#include "tab5_frame.hpp"

// Prepare the two pages. Idempotent: detects alone when the panel has been
// recreated (sleep destroys and reassembles the DSI, and the framebuffers change).
bool tab5_screen_init(void);

// Releases what was mounted. Call BEFORE destroying the panel (sleep).
void tab5_screen_shutdown(void);

// PAGE EPOCA: goes up whenever the pages are no longer valid (panel
// closed) or are born new. Read of grace - and what leaves the composer
// check "does my page still exist?" with each writing without calling anything.
extern volatile uint32_t g_tab5_screen_epoch;

// Goes up with each new panel. The composer uses it to know which pages he
// knew died (and the entire screen needs to go to the new ones).
uint32_t tab5_screen_generation(void);

// -------------------------------------------------------------------------------
// COMPOSER PRIMITIVES - no one else should use
// -------------------------------------------------------------------------------
// Page of back (true) or of front (false).
void* tab5_screen_page(bool back);

// Requests the exchange WITHOUT waiting. Caller waits with tab5_frame_wait_for_swap()
// before touching in page of back of new. [y0, y1) limits the msync of driver.
bool tab5_screen_request_swap(int y0, int y1);

// -------------------------------------------------------------------------------
// AREA DRAWING, on the target of system
// -------------------------------------------------------------------------------
// Where clear/scroll write to. The kernel points to the virtual screen in
// boot; nullptr = M5.Display (only if the virtual screen cannot exist).
void tab5_screen_set_target(lgfx::LovyanGFX* d);

// Paints one rectangle of one color. Dismisses work when the area is already
// known to be in this color (see tab5_screen_mark_clean) - whoever writes the app asks
// cleaning always, without conditions; the decision is made here.
void tab5_screen_clear(int x, int y, int width, int height, uint16_t color);

// TO THE SYSTEM: "this rectangle has just become this color". Valley
// during A pass of drawing. Prevents double cleaning (the system cleans the
// useful area and then the app asks for the same cleaning).
void tab5_screen_mark_clean(int x, int y, int width, int height, uint16_t color);
void tab5_screen_forget_clean(void);
uint32_t tab5_screen_avoided_clears(void);   // diagnostics

// Shifts the content of an area 'delta' lines (>0 goes up, <0 goes down) and paints
// with 'color' the strip exposed. Reusing already drawn pixels saves CPU in a
// scroll. false = |delta| >= height, nothing to reuse.
bool tab5_screen_scroll(int x, int y, int width, int height, int delta, uint16_t color);

// Diagnosis: how much scrolling cost the CPU since boot - displaced pixels
// for copyRect and the team spent on this (us). Accumulated, never reset: who
// measure, save a photo and make a difference.
void tab5_screen_scroll_cost(uint64_t* px, uint64_t* us);

// OBSOLETE - does nothing. It was the "budget" of writing for
// breathe between bursts in visible page. It's just because the Cobra app (tab of
// game) still calls; exit when it stops calling.
static inline void tab5_screen_spent(size_t) { }
