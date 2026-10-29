// THE SYSTEM DESIGN OBJECT (historical name: "virtual screen").
//
// A common LovyanGFX whose panel is one TRACKER: annotates each rectangle that
// any primitive of M5GFX played and delivers it to composer
// (comp_invalidar). Home, bars and all apps draw here, without knowing
// none of that.
//
// DIRECT MODE (docs/COMPOSITOR.md): the tracker writes on BACK PAGE of
// panel. The first write of each iteration opens the frame (comp_open: waits for
// previous swap and synchronize), and only then the pixel is written. Until step 3a
// this was one canvas of 1.84 MB in PSRAM that the composer copied to the
// page every frame - the copy exited, and the memory too.
//
// What remains valid, without changing a line of app:
// - no drawing happens on page that the panel is scanning;
// - changed areas are discovered ALONE.
// What changed: the content NO LONGER survives on sleep (the pages die
// with the panel). When waking up, the core redesigns everything - as it already did.

#pragma once

#include <M5Unified.h>

// Links the object of drawing to the pages of panel and makes it the target of the system
// (tab5_screen_set_target). Call once, of task of interface.
bool virtual_screen_start(void);

// The target of drawing. While it doesn't exist, it's the M5.Display.
lgfx::LovyanGFX& virtual_screen(void);

// Copies the rotation of M5.Display (the Mesh opens in landscape). The touch continues
// being converted by M5.Display; here you just need to draw the same.
void virtual_screen_follow_rotation(void);

// The rectangle (x, y, w, h), in coordinates of drawing, will be INTEGER
// overwritten by the caller, still in this frame - what the page behind
// was late there, no need to come from the front. And what fillRect already says
// alone; Suitable for those who cover with an image (pushSprite) instead of color.
void virtual_screen_cover(int x, int y, int w, int h);

// Requests the entire screen in the next frame (any task).
void virtual_screen_invalidate_everything(void);

// Returns the surface of DMA (system_surface) of the app that closed. Call
// after ao_fechar() - whoever wrote on it (the camera) has already stopped.
void virtual_screen_release_surface(void);
