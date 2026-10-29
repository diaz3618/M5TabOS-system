// DISTANCE FROM SIGNAL STRENGTH - an RSSI estimate for Wi-Fi and Bluetooth.
//
// Log-distance model, as used by "find my tracker" apps:
//
// d = 10 ^ ((P1m - RSSI) / (10 * n))
//
// P1m - expected RSSI one meter from the transmitter;
// n - environmental attenuation (2 in free space, 2.5-3.5 indoors).
//
// THIS IS A ROUGH ESTIMATE: walls, bodies, antenna orientation, and reflections
// shift RSSI by 5-10 dB, roughly doubling or halving the estimated distance.
// It distinguishes "near / in the room / far" and shows whether walking with
// the device is getting you closer; it is not a measurement. Hence the rounding.
#pragma once

#include <math.h>
#include <stdio.h>
#include <stddef.h>

// Typical P1m values when the signal does not provide one: router (~20 dBm
// transmit power) and ordinary BLE device (~0 dBm). At 2.4 GHz, loss over the
// first meter is about 40 dB.
#define TAB5_SIGNAL_P1M_WIFI   (-40)
#define TAB5_SIGNAL_P1M_BLE    (-59)
#define TAB5_SIGNAL_LOSS_1M   41     // tx_power of advertisement -> P1m
#define TAB5_SIGNAL_N_INDOOR  2.7f

static inline float tab5_signal_distance_m(int rssi, int p1m)
{
  return powf(10.0f, (float)(p1m - rssi) / (10.0f * TAB5_SIGNAL_N_INDOOR));
}

// "~40 cm", "~3 m", "~15 m", "> 50 m" - rounded to precision the estimate
// can actually support.
static inline void tab5_signal_distance_text(int rssi, int p1m, char* out, size_t size)
{
  const float d = tab5_signal_distance_m(rssi, p1m);
  if      (d < 1.0f)  { snprintf(out, size, "~%d cm", ((int)(d * 10.0f + 0.5f)) * 10); }
  else if (d < 10.0f) { snprintf(out, size, "~%d m", (int)(d + 0.5f)); }
  else if (d < 50.0f) { snprintf(out, size, "~%d m", ((int)(d / 5.0f + 0.5f)) * 5); }
  else                { snprintf(out, size, "> 50 m"); }
}
