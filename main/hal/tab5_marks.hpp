// SYSTEM MILESTONES - "what happened, and when", to intersect with the frame
// late.
//
// The delayed frame has a fixed signature: one entire frame lost
// (~30 ms with period of 15), with BOTH cores idle - that is, it is not
// software delaying the ISR, and the DMA of panel being undated. What's missing
// and know what the HARDWARE was doing at that moment. Each suspect
// leaves one mark here, and the monitor, on seeing one delay, lists the marks that
// fell just before him.
//
// Suspect number 1 registers alone: ALL CPU clock changes made
// by the power manager (locks of PM of UART, I2C, PPA; esp_pm_configure)
// goes through rtc_clk_cpu_freq_set_config_fast() - and BEFORE it the IDF readjusts
// the mode of bus of PSRAM/flash (esp_clk_utils_mspi_speed_mode_sync_*).
// Link forwards this call to ca (-Wl,--wrap, main/CMakeLists.txt).
//
// Ring in INTERNAL RAM (the bypass runs with the cache in any state, within
// critical section): 32 marks, text always LITERAL (only the pointer is saved).
#pragma once

#include <stdint.h>

// Leaves one mark. 'what' has to be literal. Callable of task and of ISR.
void tab5_mark(const char* what, int32_t value);

typedef struct {
  int64_t     us;      // esp_timer_get_time()
  const char* what;
  int32_t     value;
} tab5_mark_t;

// The marks between [from_us, to_us], from oldest to newest. Return how many.
int tab5_marks_between(int64_t from_us, int64_t to_us, tab5_mark_t* out, int max);
