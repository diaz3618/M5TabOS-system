// Painting by difference - the help that apps were missing.
//
// ------------------------------------------------------------------------
// THE PROBLEM THIS SOLVE
// ------------------------------------------------------------------------
// M5GFX draws directly to the visible framebuffer, so ERASE TO REDRAW
// appears as flash. The rule is write by top, with fixed width, and so
// when the value changed.
//
// But the rule alone was not enough: it was forgotten in five apps (Energy,
// Tasks, Adjustments, Files and Mesh), each one fixed separately. And
// always came with the same second error: forgetting that the SYSTEM cleans the area
// (in opening, on returning from sleep) and from there the app needs to repaint EVERYTHING,
// otherwise the screen stays blank - that's what happened with the Settings app.
//
// As long as this is programmer memory, it will continue to fail. Here the
// two steps are embedded: painting_initiar() queries the cleaning of area
// alone, and in a complete pass EVERY comparator returns "changed" - then the
// app repaints everything without having to remember anything.
//
// ------------------------------------------------------------------------
// HOW TO USE IT
// ------------------------------------------------------------------------
// static painting_t s_p;
// static int s_bat; static char s_name[24];
//
// void draw(lgfx::LovyanGFX& d, const area_t& a)
// {
// painting_start(&s_p);                       // decide whether and complete
// if (complete_painting(&s_p)) { ...background, titles, frames... }
//
// if (pintura_mudou_i(&s_p, &s_bat, battery)) { ...draws the number... }
// if (pintura_mudou_s(&s_p, s_nome, sizeof(s_nome), name)) { ... }
// }
//
// // on switch of page, of selection, of anything structural:
// painting_order(&s_p);  system_request_complete_redesign();

#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  bool complete;    // this pass repaints everything
  bool request;      // the app requested a complete repaint
  bool ja_desenhou; // false until the first pass (which is always complete)
} paint_t;

// Call at the BEGINNING of draw(). Decide whether to pass and complete, looking: if
// and the first, if the app requested it, or if the system cleaned the area.
void paint_start(paint_t* p);

// The app requires complete repainting. Use on change of page, of selected item or
// anything that rearranges the screen - and also calls
//sistema_pedir_redesenho_completo(), which is who schedules the drawing.
void paint_request(paint_t* p);

static inline bool paint_complete(const paint_t* p) { return p->complete; }

// JUST TO DECIDE DESIGN. Do not use these comparators to decide I/O (reread
// card, open file, talk to hardware): they return true in every pass
// COMPLETE, by drawing, and an I/O that triggers repaint complete goes into a loop
// was what made the virtual keyboard of app Mesh blink by itself.
// For I/O decision, compare in hand.
//
// Comparators WITH MEMORY. They store the value and return true when it changes -
// and ALWAYS true in a pass complete, which is what makes the app repaint everything without
// need to remember that.
bool paint_changed_i(const paint_t* p, int* memory, int now);
bool paint_changed_s(const paint_t* p, char* memory, size_t size, const char* now);
