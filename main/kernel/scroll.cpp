#include "scroll.hpp"
#include "tab5_screen.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void scroll_set(scroll_t* r, uint8_t eixo, uint8_t anchoring)
{
  r->current = r->no_visor = r->max = 0;
  r->anchor = r->press_start = -1;
  r->distance = 0;
  r->eixo = eixo;
  r->anchoring = anchoring;
  r->passo_encaixe = 0;
}

void scroll_reset(scroll_t* r)
{
  r->current = 0;
  r->max = 0;   // consumer that stores 'max' between passes (the Mesh) does not
                // stays with an old limit of a previous screen/conversation on
                // instant between this and the next drawing recalculate; who
                // recalculates 'max' EVERY cycle (the kernel) doesn't even notice.
  r->anchor = -1;
  r->press_start = -1;
  r->distance = 0;
}

bool scroll_drag(scroll_t* r, int coord, bool dentro)
{
  // RELEASE THE ANCHOR WHEN LEAVING THE ZONE, not only when the finger lifts again -
  // unified default in consolidation (the three lists of Mesh
  // they already did it like that; the kernel previously only released on '!touching', tolerating the
  // finger wander outside the useful area without letting go of the anchor - one finger that leaves
  // and came back, I jumped the whole distance once, because the anchor was
  // stop at the last point IN. Releasing on exit avoids this jump, and the
  // behavior that already applies to most lists in this system.
  if (!dentro) { r->anchor = -1; return false; }

  // First sample within zone: only anchor, there is no 'previous cycle'
  // to take no delta - avoid this and what prevents one from jumping on
  // ENTER zone (the finger may have come from far outside it).
  if (r->anchor < 0) { r->anchor = coord; r->press_start = coord; r->distance = 0; return false; }

  const int dy = coord - r->anchor;
  if (dy == 0) { return false; }
  r->anchor = coord;

  // CLEARANCE OF THE DESCENT, and not the sum of the path traveled - one finger that
  // shakes (or goes and iteration) stopped cannot be read as large drag only
  // for having "walked" a lot back and forth. Same historical kernel logic.
  const int longe = (coord > r->press_start) ? (coord - r->press_start) : (r->press_start - coord);
  if (longe > r->distance) { r->distance = longe; }

  // SIGNAL: common list goes up/down with the finger (finger to top = content
  // goes up, so it subtracts); anchored at the end does the opposite (finger to bottom
  // reveals older content, so it adds).
  int nova = (r->anchoring == ROLAGEM_INICIO) ? (r->current - dy) : (r->current + dy);
  if (nova < 0)      { nova = 0; }
  if (nova > r->max) { nova = r->max; }
  if (nova == r->current) { return false; }

  r->current = nova;
  return true;
}

bool scroll_foi_arraste(scroll_t* r)
{
  const bool foi = r->distance > ROLAGEM_ARRASTE_MIN;
  r->distance = 0;
  return foi;
}

bool scroll_limit(scroll_t* r, int total, int window)
{
  int novo_max = total - window;
  if (novo_max < 0) { novo_max = 0; }
  r->max = novo_max;
  if (r->current > r->max) {
    // The list shrank (one item was removed, a conversation changed) and the
    // position current became beyond end - pulls of iteration.
    r->current = r->max;
    return true;
  }
  return false;
}

scroll_pass_t scroll_scroll(scroll_t* r, const area_t& window, uint16_t color)
{
  scroll_pass_t p = {};
  p.delta = (r->anchoring == ROLAGEM_INICIO) ? (r->current - r->no_visor)
                                             : (r->no_visor - r->current);
  p.moved = tab5_screen_scroll(window.x, window.y, window.width, window.height, p.delta, color);
  if (p.moved) {
    const int ad = (p.delta > 0) ? p.delta : -p.delta;
    p.exposta = area_t{
      window.x,
      (p.delta > 0) ? (window.y + window.height - ad) : window.y,
      window.width, ad
    };
  }
  // ALWAYS synchronizes, whether moved or not: if not moved, the caller
  // will clear/redraw the entire window on its own (each one of
  // your way - see the big warning in the .hpp), and after that the page
  // visible ALSO starts to reflect 'current', but in another way.
  r->no_visor = r->current;
  return p;
}

void scroll_barra(lgfx::LovyanGFX& d, const scroll_t* r, const area_t& window,
                   uint16_t color_track, int margem)
{
  if (r->max <= 0) { return; }
  // Extra gate only for anchored-to-end: with current==0 the "end" is already
  // view, the little bar there would be noise (same rule as messages of Mesh
  // already had it before this consolidation).
  if (r->anchoring == ROLAGEM_FIM && r->current <= 0) { return; }

  const int bx  = window.x + window.width - margem;
  const int alt = (window.height * window.height) / (window.height + r->max);
  const int pos = (r->anchoring == ROLAGEM_INICIO) ? r->current : (r->max - r->current);
  const int by  = window.y + ((window.height - alt) * pos) / r->max;

  d.fillRect(bx, window.y, 4, window.height, color_track);
  d.fillRoundRect(bx, by, 4, alt, 2, TFT_DARKGREY);
}
