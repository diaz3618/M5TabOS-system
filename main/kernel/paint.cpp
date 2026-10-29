#include "paint.hpp"
#include "app.h"

#include <string.h>

void paint_start(paint_t* p)
{
  // The order matters: we consultsistema_area_foi_limpa() ALWAYS, even if it is already
  // let us know that the pass is complete, because it is only valid during this
  // call of draw() - not of to postpone reading.
  const bool limpou = system_area_foi_limpa();
  p->complete = limpou || p->request || !p->ja_desenhou;
  p->request      = false;
  p->ja_desenhou = true;
}

void paint_request(paint_t* p) { p->request = true; }

bool paint_changed_i(const paint_t* p, int* memory, int now)
{
  if (p->complete || *memory != now) { *memory = now; return true; }
  return false;
}

bool paint_changed_s(const paint_t* p, char* memory, size_t size, const char* now)
{
  if (p->complete || strncmp(memory, now, size - 1) != 0) {
    snprintf(memory, size, "%s", now);
    return true;
  }
  return false;
}
