// Touch scrolling - the SYSTEM mechanism, and no longer just one app.
//
// ------------------------------------------------------------------------
// WHY DOES THIS EXIST
// ------------------------------------------------------------------------
// There were TWO scrolling mechanisms, unrelated to none
// each other: the generic of core (kernel/core.cpp, single consumer or
// Settings) and the Mesh app itself - THREE almost identical copies (conversations,
// in,messages), each with its own drag/offset pair
// handwritten. The three copies of Mesh never gained the 80ms brake that
// the Settings app has always had it (see the big warning in kernel/core.cpp about
// REPINTURA_MIN_MS): the touch of Mesh falls on the only branch of the main loop
// without throttle, and redraws at ~50x/s instead of ~12.5x/s.
//
// Design criterion: not patching just the Mesh (it would be a patch for one app
// so) - the SYSTEM needs ONE mechanism, which the kernel and
// any app you use, and that serves as a base for what you will still need
// scrolling in the future (the home itself, paged as screen of cell phone, when
// fill of apps).
//
// ------------------------------------------------------------------------
// WHO USES
// ------------------------------------------------------------------------
// This is the ENGINE. It has two consumers, and there are only two of them:
// - kernel/list - ALL scrollable list of app (Mesh, Files, Tasks).
// The app never calls scroll_* directly: it uses list_*.
// - kernel/core - scrolling of entire area (app with content_height,
// the Settings app).
// An app that needs to scroll something that is not a list (one map, one page
// horizontal) is a sign that a COMPONENT is missing in the kernel, not that the app
// must create its own roll with these functions.
//
// ------------------------------------------------------------------------
// WHAT THIS DOESN'T DO
// ------------------------------------------------------------------------
// Doesn't know anything about pages, columns, keyboard open or any other rule
// of zone - this is of caller (see 'inside' in scroll_drag). No
// you decide FULL vs LIGHT - this remains the decision of each app (EVERYTHING,
// painting_complete(), etc). Don't touch the framebuffer alone outside of
// scroll_dislocar()/roll_bar() - the rest is just accounting.

#pragma once

#include <stdint.h>
#include <M5Unified.h>
#include "app.h"   // area_t

// AXIS and ANCHORAGE are SEPARATE fields, and not a single enum of four
// values - are orthogonal questions. "Vertical or horizontal" decides WHICH
// function of offset serves (tab5_screen_scroll copies entire lines: and
// vertical by construction). "Start or end" decides the SIGN of drag and the
// position of bar. The horizontal pagination of home, one day, will need
// of first field without touching the second - packaging both would force
// invent a value like "HORIZONTAL_FIM" that nobody wants, just for the enum
// close.
#define ROLAGEM_VERTICAL    0
#define ROLAGEM_HORIZONTAL  1   // reserved - see the warning of pagination at the end of this file

#define ROLAGEM_INICIO  0   // common list, read from top (the default - so it is zero)
#define ROLAGEM_FIM     1   // anchored at end (ex: messages - 0 = most recent)

// Minimum distance from point of press_start of finger to count as drag (and
// don't click). The SAME number for the entire system: the kernel (click on
// core.cpp) and every list (kernel/list) decides "it was touch or drag" by
// him.
//
// 24 px (~2 mm at 294 DPI). There were 12, and the kernel started to use
// 24 new click recognizer - with two limits, one touch that moved
// 13..24 px passed through the kernel and LIST rejected it as "end of drag":
// in the Files app, touch after touch with no effect and no log. Before the
// wasClicked() of M5Unified refused everything above 8 px and the limit of list
// never came into play. Just one number prevents this from happening again.
#define ROLAGEM_ARRASTE_MIN 24

typedef struct {
  int current;         // current offset, in pixels; 0 = in origin of anchoring
  int no_visor;      // as for the VISIBLE PAGE already reflects of 'current'
  int max;           // limit; 0 = does not work. Who records and the caller (limit_scroll
                      // recalculates itself when the content may shrink)
  int anchor;        // coordinate of finger in the previous cycle; -1 = no finger
  int press_start;        // coordinate WHERE the finger went down - the reference of drag
  int distance;    // greatest distance of press_start until now (drag vs. click)
  uint8_t eixo;        // SCROLLING_VERTICAL/HORIZONTAL
  uint8_t anchoring;   // SCROLLING_START/END
  int passo_encaixe;   // 0 = free scrolling. Reserved for future pagination
                       // (fit 'current' into a multiple on release or finger) -
                       // has no function yet because there is no screen to sense it.
} scroll_t;

// What A pass of scroll_shift() did - the caller uses it to crop
// (only the new track needs to be redrawn).
typedef struct {
  int    delta;      // offset applied, in space of TELA (which was
                      // for tab5_screen_scroll - already with the right sign for
                      // anchoring this list)
  bool   moved;    // took advantage of the pixels that were already right
  area_t exposta;     // the newly exposed track; only validates when moved==true
} scroll_pass_t;

// Prepares a roll with explicit axis/anchoring. Just needs to be called
// for the NON-standard case (ROLAGEM_END, or one day HORIZONTAL) - one
// zero-initialized scroll_t (static, global) is already born VERTICAL/INICIO.
void scroll_set(scroll_t* r, uint8_t eixo, uint8_t anchoring);

// Return 'current' to origin and release an anchor of finger - call on open a
// new screen or switch of selected item. DO NOT touch 'no_visor': who
// zero will also force a pass complete (which synchronizes alone via
// scroll_synchronize), exactly as the kernel already did.
void scroll_reset(scroll_t* r);

// "The page visible now reflects 'current'." Call after any
// COMPLETE pass (which redesigned everything from scratch, so there is nothing left
// pending of scroll).
static inline void scroll_synchronize(scroll_t* r) { r->no_visor = r->current; }

// True when the visible page does not yet reflect the current offset - the
// sign that a pass of rolagem_dislocar() has work to do.
static inline bool scroll_pending(const scroll_t* r) { return r->current != r->no_visor; }

// A sample of touch. 'coord' is the coordinate on the AXIS of this roll (y for
// vertical), already in the caller space - this module knows nothing about zones,
// pages or keyboard open, this is the responsibility of those who calculate
// 'inside'. Returns true SO when 'current' changed of true (clamped in
// [0, r->max]) - one touch stopped or outside of zone never returns true.
bool scroll_drag(scroll_t* r, int coord, bool dentro);

// "The last touch was DRAG, not click?" Compare the maximum distance
// (accumulated by scroll_drag) against SCROLL_DRAG_MIN and ZERO the
// distance - call once per release of finger (e.g. within treatment
// of wasClicked()), never more than once per touch.
bool scroll_foi_arraste(scroll_t* r);

// Recalculates 'max' based on how much content there is (total) and how much it fits
// in window visible, and PULL 'current' of iteration if it became beyond new
// limit (the list shrank - one item was removed, one conversation changed).
// Returns true when it was necessary to pull, for the caller to mark the area as
// dirty if applicable.
bool scroll_limit(scroll_t* r, int total, int window);

// Shifts the correct pixels within 'window' (tab5_screen_scroll) to
// until the visible page reaches 'current'. Synchronizes 'no_visor' alone,
// ALWAYS - moved of true (moved==true) or not (caller will clear
// and redesign everything on its own, and then 'no_visor' has to follow
// in the same way, otherwise the next pass would think there is still a pending issue).
// DOES NOT clean anything alone when moved==false: the fallback is the decision of
// each caller (kernel drops to a FULL repaint via page
// hidden; Mesh clears the window in time, in visible page) - both
// ways are legitimate and different, and should not be forced on one.
scroll_pass_t scroll_scroll(scroll_t* r, const area_t& window, uint16_t color);

// The bar: track + dot, 4px of width a 'margin' px of edge
// right of window. Who draws is the list (the Adjustments app does not have a bar, request
// ).
void scroll_barra(lgfx::LovyanGFX& d, const scroll_t* r, const area_t& window,
                   uint16_t color_track, int margem = 8);


// ------------------------------------------------------------------------
// FUTURE PAGINATION (not implemented - only drawing does not prevent it)
// ------------------------------------------------------------------------
// When the home needs horizontal pages: ROLLAGEM_HORIZONTAL +
// ROLAGEM_INICIO (not one fifth value of anchoring), 'passo_encaixe' > 0
// (the width of a page), is a new function - scroll_soltar(r), called
// at the end of drag - which fits 'current' in the multiple of 'passo_encaixe'
// more next. scroll_dislocar() would dispatch in 'axis' (it would need one
// tab5_tela_rolar_h() in HAL, since of today copies entire lines and e
// vertical by construction). There is still no screen that feels this way, so
// there is nothing to test - that's why the function does not exist today, only the field.
