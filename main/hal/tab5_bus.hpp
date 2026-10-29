// MEMORY-BUS PRIORITY (ESP32-P4 AXI-ICM).
//
// THE ROOT PROBLEM. The screen is a MIPI-DSI panel in DPI mode: the controller
// reads the PSRAM framebuffer CONTINUOUSLY, line by line, at the pixel-clock
// rate. At 80MHz for a 720x1280 panel including porches, that is about 119 MB/s
// which CANNOT be delayed; every line must be in the FIFO when requested.
//
// When the processor also hammers PSRAM (clearing the screen, drawing bubbles,
// writing back 256KB of cache), both contain for the same bus. If screen DMA
// loses for too long, the FIFO empties. IDF's own driver says what happens:
//
// "when an underrun happens, the LCD display may already become blue,
// it's too late to recover the display, so we just print an error message
// as a hint to the user that he should optimize the memory bandwidth
// (with AXI-ICM)" (esp_lcd_panel_dpi.c, line 113)
//
// THAT IS THE BLUE FLICKER. It is not an app bug; it is bus arbitration.
//
// WHAT WE HAD BEEN DOING WAS A WORKAROUND. Clearing in strips with pauses,
// avoiding duplicate clears, and spacing out repaints all REDUCE CPU-side
// demand and help. They of not solving it: every new app must remember every
// rule, while the underlying contention remains.
//
// THE SOLUTION THE DRIVER ITSELF POINTS TO is AXI-ICM. The interconnect matrix
// has PER-MASTER arbitration priorities, all 0 by default, so screen DMA is
// treated exactly like the processor cache. Giving the panel's master higher
// priority makes the arbiter serve the screen first, preventing FIFO starvation
// regardless of what the CPU is doing.
//
// DPI reads through DW-GDMA (esp_lcd_panel_dpi.c uses esp_private/dw_gdma.h),
// which appears in ICM as gdma_mst1 and gdma_mst2.
//
// HONESTY ABOUT THE RISK: no IDF component calls this exposed API. I couldn't
// test it on hardware. That is why a console command can enable and disable it
// at runtime: both states can be compared without reflashing, and the driver's
// "underrun happens" log immediately tells us which is right.
#pragma once

#include <stdbool.h>

// Applies the priority once at boot.
void tab5_bus_prioritize_screen(bool enable);

// Current state for display by the console.
bool tab5_bus_is_prioritized(void);
