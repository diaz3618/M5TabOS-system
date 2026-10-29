// FRAME HAL - the lowest layer of new base of screen (docs/COMPOSITOR.md,
// step 1). Two pieces of hardware that the old base never used.
//
// (The priority of screen on the AXI bus DOES NOT live here: and of
// hal/tab5_bus, enabled at boot. A copy of it lived here for a few
// hours and fought with the original.)
//
// 1. THE END OF THE FRAME. The driver only changes the page when the DMA of panel
// restarts (mipi_dsi_dma_trans_done_cb le cur_fb_index at that moment), and
// warns for on_frame_buf_complete. Wait for this warning after request a
// exchange is what guarantees that the page that exited is no longer being read -
// then just write to it without tearing the image.
//
// 2. COPY BY HARDWARE (pure DMA2D, esp_async_fbcpy; the PPA is of reserve).
// Copy one rectangle of one page to another without the CPU, in line
// run - 3x faster than the SRM of PPA engine (measured: 'screen bank').
// And how the changed areas of one frame are taken to the other page.
//
// And one counter of UNDERRUN, because without measurement "improved" is just an opinion.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_lcd_types.h"
#include "freertos/FreeRTOS.h"

// Rectangle in PHYSICAL coordinates of framebuffer (portrait, 720x1280).
typedef struct {
  int16_t x, y, l, a;
} tab5_rect_t;

// Connects the three pieces on panel current. Idempotent by handle: sleep
// destroys and recreates the panel, and the next call registers it again.
bool tab5_frame_init(esp_lcd_panel_handle_t panel);

// Forget the panel. Call on destroy the panel (sleep).
void tab5_frame_shutdown(void);

// Discards an end of frame warning that arrived earlier. Call LOGO
// AFTER request the change - see why in tab5_frame.cpp.
void tab5_frame_arm_wait(void);

// Wait for the panel to start scanning the new page. false = deadline has passed.
bool tab5_frame_wait_for_swap(uint32_t timeout_ms);

// Copies the rectangle 'r' of 'source' to the SAME place of 'target' (images
// 720x1280 RGB565: the panel pages and the virtual screen), by DMA2D.
// Blocks until finished.
bool tab5_frame_copy(void* target, const void* source, const tab5_rect_t& r);

// Same, with ORIGIN elsewhere: the rectangle 'r' of target receives the
// rectangle of same size that starts at (ox, oy) in source. And the scroll
// by DMA: the front page, moved, becomes the back page. Origin and target
// CANNOT be the same buffer (DMA does not handle overlap).
bool tab5_frame_copy_from(void* target, const tab5_rect_t& r, const void* source, int ox, int oy);

// Diagnosis.
uint32_t tab5_frame_underruns(void);   // notices of underrun since boot
uint32_t tab5_frame_count(void);     // frames scanned by panel

// Frames that DELAYED (interval between ends of frame 50% above period
// normal) since boot. worst_us = the largest range since a latest call
// (and resets); period_us = the measured normal period.
uint32_t tab5_frame_delays(uint32_t* worst_us, uint32_t* period_us);

// When (uptime, us) the panel finished scanning the last frame. With the
// period above, of a PHASE of sweep: who anima can align with it
// (lock screen). 0 = panel has not scanned anything yet.
int64_t tab5_frame_last_end_us(void);

// The last delay: on which core the ISR of panel runs, and what task was
// running on each core when it was finally served (t0/t1: 16 bytes).
// Returns a counter that increases with each registered delay.
uint32_t tab5_frame_delay_source(int* isr_core, char* t0, char* t1);
int64_t  tab5_frame_delay_time(void);   // uptime (us) of last delay

// SAFE WINDOW FOR LONG CRITICAL SECTION. Between two frames the
// DMA of panel has ~0.5 ms to be reconnected by the ISR - and a critical section
// (interrupts turned off in the core) that falls into this gap loses one frame
// integer. It was the cause of "FRAME DELAYED" with both colors in IDLE: a
// photo of monitor tasks (uxTaskGetSystemState, ~0.8 ms with CPU at 360)
// ran 1x/s in fixed phase and from time to time it fell well into slack - all
// delays had the same fraction of seconds (x.728 s).
//
// This function iteration just AFTER one end of frame (the ISR has already reconnected the DMA):
// from then on there are ~14 ms until the next gap. Who will stay in the critical section for
// more than ~100 us calls this before. No panel (sleep), iteration on time.
bool tab5_frame_wait_for_end(uint32_t timeout_ms);

// Errors of DSI host read every frame: returns how many frames there were
// some bit since boot; st0/st1 = OR of the bits since the latest call (and resets).
uint32_t tab5_frame_dsi_errors(uint32_t* st0, uint32_t* st1);

// HARDWARE FILLING (PPA, FILL operation): paints the rectangle 'r' of
// 'target' (720x1280 RGB565) with 'color' (RGB565 as stays in memory), without the
// CPU. Block until finished. false = unavailable or refused - who called
// paints by CPU. The first time, check the written pixel; if the color is not
// hit (order of bytes), turns off by itself and reports in the log.
bool tab5_frame_fill(void* target, const tab5_rect_t& r, uint16_t color);

// COPY TEST BENCH. Copy the same rectangle between two buffers
// temporary in PSRAM (allocated and returned here) for each engine
// available - PPA (SRM), pure DMA2D, GDMA AXI and CPU - with panel scanning
// as always, and prints ms and MB/s of each one. And the measurement that chooses the engine
// of copies between pages. Call of console; takes ~1 s.
void tab5_frame_benchmark(void);
