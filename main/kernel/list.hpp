// SCROLLABLE LIST - the first default component of new base (docs/COMPOSITOR.md,
// step 3b). Same idea of list of LVGL and of any cellular system:
//
// THE ITEM IS MEASURED ONCE, WHEN IT CHANGES. THE LIST SAVES THE RESULT.
//
// The app only tells you three things: how many items there are, the height of each one (which
// he calculated and saved) and how to draw item i in position y. The list
// takes care of the rest - scrolling, limit, height sum, clipping, bar of
// scrolling, and the cheap way of scrolling: scroll what is already drawn in
// virtual screen and draw SO the newly exposed track, and only with the items that the
// cross.
//
// Before this, each system list reimplemented this core, and the Mesh
// mediates ALL messages at every step of scrolling (line break, emoji,
// mutex of service) just to discover pitches that hadn't changed.
//
// Usage (EVERY system scrollable list follows this - see docs/WRITING_APPS.md):
// static list_t s_l;
// // on open: list_define(&s_l, ROLLAGEM_INICIO, n_items, height_of, draw_item, ctx);
// // items changed (entered, exited, re-measured):lista_itens_mudaram(&s_l, n);
// // UM item changed of value (age, counter): list_item_changed(&s_l, i);
// // ao_tocar_nivel: if (lista_drag(&s_l, x, y, pressed))sistema_request_redesenho();
// // when_touched: const int i = list_item_touched(&s_l, x, y, &dy);   // -1 = nothing/drag
// // draw: list_desenhar(&s_l, d, window, background, everything);
//
// THE RINGER IS ALSO FROM THE LIST. Before each app decided "the finger is
// inlist?" and "which item is this?" with its own account - the same geometry
// written twice (in drawing and in touch), with scrolling added by hand. It was
// exactly as soon as the neighboring item has already been opened and a hidden column
// on the keyboard kept scrolling at the bottom of it. Now the list uses the window of
// LAST drawing: what she didn't draw, she doesn't react to.
//
// The column of bar (8px to the right of window) and of list: do not draw items
// there.

#pragma once

#include "scroll.hpp"

typedef int  (*list_height_t)(int i, void* ctx);
typedef void (*list_draw_t)(lgfx::LovyanGFX& d, int i, int x, int y, int width, void* ctx);

typedef struct {
  scroll_t        rol;
  int              n;
  list_height_t   height;
  list_draw_t draw;
  void*            ctx;

  int              topo;   // breath before item 0 (px) - rolls along

  // Internal state.
  int     total;         // top + sum of heights (validates if measurement_ok)
  bool    measurement_ok;
  bool    everything;          // next pass redraws the entire window
  area_t  window_ant;    // to notice that the window changed (keyboard opened)
  bool    no_ar;         // drawn on this screen (janela_ant applies to touch)
  int     dirty_of, dirty_ate;   // items to redesign alone; of > ate = none
} list_t;

// Prepare the list. ROLAGEM_INICIO = read of top; ROLAGEM_FIM = anchored in
// end (messages: 0 = most recent, pasted at the bottom).
void list_set(list_t* l, uint8_t anchoring, int n,
                   list_height_t height, list_draw_t draw, void* ctx);

// The items changed (entered, exited, some were re-measured). Redesign everything in
// next pass. In a list anchored at the END, read in the middle of history,
// what GREW goes below without dragging what you are reading.
void list_items_changed(list_t* l, int n);

// An item changed of VALUE without changing of height (age of one no, counter of no
// read). Only it is redrawn in next pass - and only if it is visible.
void list_item_changed(list_t* l, int i);

// Returns to the origin (top, or the end of a list anchored at the end).
void list_on_start(list_t* l);

// The screen that showed the exited list (another app page): it stops reacting
// on touch until drawn again.
void list_hide(list_t* l);

// Draw what you need: the entire window is 'everything' (or if something changed), just the
// range that the scrolling exposed, or are the items marked with list_item_changed.
// Returns true if you painted something.
bool list_draw(list_t* l, lgfx::LovyanGFX& d, const area_t& window,
                    uint16_t fundo, bool everything);

// TOUCH, in coordinates of AREA of app (those that ao_tocar/ao_tocar_nivel
// receive). true = rolled (system_request_redesign request).
bool list_drag(list_t* l, int x, int y, bool pressed);

// The item under the finger at the end of one touch; -1 = out of list or in an empty place;
// LIST_ARRASTE = the touch was the END of one drag in this list (drag is not
// click - the app should not treat anything, not even a button, as a touch). dy = where,
// inside of item, the finger dropped (0 = top of item), for buttons inside of item.
#define LISTA_ARRASTE (-2)
int  list_item_touched(list_t* l, int x, int y, int* dy);
