// LOCK SCREEN ANIMATIONS.
//
// Each time the lock screen appears, a different animation will appear.
// is drawn and stays running BEHIND the texts until unlocked.
//
// Rules that make this light:
// - the state of all animations shares a single static union
// (~2 KB); only the lucky one is alive;
// - nothing about repainting the screen: each object only erases its own box (the old one
// and the new one in one) and it is designed new. All forms are without
// antialiasing, so drawing twice in the same place is harmless;
// - boxes that overlap become one (nothing is deleted twice);
// - the frame exit in the rhythm of frame of system (kernel/frame_pacing.hpp), like the
// of any periodic app, and its system_quadro_dt() is used;
// - the background (abstract lines) and an image painted once per block
// (PSRAM, only while locked): "erase" a box and copy the chunk
// of it, the same cost of black paint;
// - the text (time, date, warning, hint) stays in front: the shell redraws it,
// cropped, just where the animation played.
//
// Call so of task of interface, of inside ofdraw_content().

#pragma once

#include <M5Unified.h>
#include "app.h"

typedef struct {
  int16_t x, y, w, h;
} bq_ret_t;

// Choose the animation of next display (never the same two times in a row).
void bq_anim_sortear(void);

// The request period while locked (~30 q/s). The system rounds it to
// entire of panel scans (2, at any clock).
static const uint32_t BQ_ANIM_PERIODO_MS = 33;

// Returns background memory (unlock, sleep). The next exhibition
// paints another one.
void bq_anim_release(void);

// The animation drawn in a newly cleaned area (black) begins.
void bq_anim_start(const area_t& a);

// Go ahead and draw one frame. 'extras' are rectangles that the shell just made
// need erase (text that changed): the animation erases them and recreates them.
// 'boxes' are text boxes; for each one, 'clips' returns the area (already
// erased, w == 0 if the animation did not play) where the text should be repainted -
// ONCE, cut into it.
void bq_anim_frame(lgfx::LovyanGFX& d, const bq_ret_t* extras, int n_extras,
                    const bq_ret_t* caixas, int n_caixas, bq_ret_t* clips);

// HOME WALLPAPER: one of the abstract background designs (style 0..4:
// threads, waves, web, rays, circles - an order of SETTINGS_WALLPAPER_*), painted
// in area 'a'. The same seed of always the same drawing. false = out of memory
// or invalid style (the area stays as it was - black).
bool bq_fundo_parede(lgfx::LovyanGFX& d, const area_t& a, int estilo, uint32_t semente);
// The ready wallpaper remains saved (PSRAM, the size of area) while
// style, seed and area are the same - the home iteration without generating it new.
// Release: on open one app and no sleep (kernel calls).
void bq_parede_release(void);
