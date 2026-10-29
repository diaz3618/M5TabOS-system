// TIME SYNC - adjusts the RTC (and the system clock together, see
// tab5_rtc.hpp) from the BEST source currently available.
//
// ------------------------------------------------------------------------
// THE SOURCES, from the most reliable to the least
// ------------------------------------------------------------------------
// NTP (internet, with WiFi connected) - server of truth, error
// of milliseconds. It goes up alone the first time the WiFi picks up IP.
//
// GPS (tab5_gps_time()) - the receiver has its own clock, synchronized
// via satellites. It only applies WITH FIX: before that, many receivers send the
// time of their internal clock, which can be anything.
//
// Mesh (tab5_mesh_hora_viva()) - the clock of in Meshtastic, read from rx_time
// of package that arrived NOW. It does not count the last_heard of list of nodes nor the
// which was stuck in queue of no during the connection: it's hours of PAST,
// and used as "now" delay the clock by hours or days - a message
// sent in this interval and postmarked with the wrong date.
//
// ------------------------------------------------------------------------
// CONSISTENCY
// ------------------------------------------------------------------------
// - Source better than current gets it right in time; the same source, every
// RESYNC_MS; a WORSE font only after the best one disappears for
// VALIDITY_MS. So Mesh never undoes what NTP or GPS got right.
// - SALTO: when the hit changes the clock by more than SALTO_S, who stamped
// things with the old time and warned (tab5_time_on_jump) and fixes the
// own stamps of this session - the message sent before the correct exit
// of wrong day along with the clock.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "esp_timer.h"
struct tm;   // forward-declared only for the pointer of tab5_local_time() -
             // whoever uses struct tm of actually includes <ctime> on their own.

// Check the sources and adjust the RTC when the rule of consistency above
// leave it. Call 1x/s, FROM THE INTERFACE TASK - and who can write to the RTC
// (see tab5_rtc.hpp).
void tab5_time_sync(void);

// Where did the last hit of this session come from: "internet", "GPS", "Mesh", or
// "internal clock" (no source yet - worth what the RTC kept).
const char* tab5_time_source(void);

// Warning of SALTO: 'delta_s' and how much the clock moved (positive = went to
// front). Called of task of interface, right after of hit; who
// signs only correct their own stamps and iteration (no card, no waiting
// long). Up to 4 subscribers.
typedef void (*tab5_time_jump_cb)(int32_t delta_s);
void tab5_time_on_jump(tab5_time_jump_cb cb);

// ------------------------------------------------------------------------
// TIMEZONE.
// ------------------------------------------------------------------------
// User-configurable whole-hour offset from UTC (Settings > About), in DST
// logic - if that's ever needed, this is where it goes, not scattered
// across apps.
//
// DISPLAY ONLY. The RTC, sync, and logs stay in pure UTC on purpose - it's
// the only way to compare timestamps between mesh nodes, log files, and GPS
// without ambiguity. Only code that's about to SHOW the time calls this -
// gmtime_r() directly is still correct for any other use.
int32_t tab5_utc_offset_s(void);

// HAS THE CLOCK BEEN SET? Below this (2020-09-13) an hour is not of
// true: RTC without hit (no GPS, network or mesh since boot) or one
// old stamp engraved as 0. It was the same number written in twelve places,
// with four different names - one so, here.
#define TAB5_EPOCH_VALIDO 1600000000u
static inline bool tab5_time_is_valid(uint32_t epoch_utc) { return epoch_utc > TAB5_EPOCH_VALIDO; }

// NOW, in epoch UTC - or 0 while the clock has not been set. The 0 and the
// "no date" that screens, files and Meshtastic already know how to handle.
static inline uint32_t tab5_time_epoch(void)
{
  const time_t t = time(nullptr);
  return (t > (time_t)TAB5_EPOCH_VALIDO) ? (uint32_t)t : 0;
}

// Converts UTC epoch to struct tm in the LOCAL TIMEZONE (tab5_utc_offset_s())
// - same format as gmtime_r() (which is what it does underneath, just on the
// already-offset epoch).
void tab5_local_time(uint32_t epoch_utc, struct tm* out);

// DAY FILE NAME: "<prefix>YYYY-MM-DD<suffix>", on LOCAL day - the day
// of those who use it. Every daily system file (log, power, monitor) uses
// this, for those of the same day to turn together at midnight (with each one doing
// your way, the log in UTC turned the day 3 hours before the others).
// Only the NAME and location: the content remains stamped in UTC.
// Clock still not set (epoch before 2020): "<prefix>no-date<suffix>".
// tab5_day_filename(time(nullptr), "log_", ".txt", name, sizeof(name));
void tab5_day_filename(uint32_t epoch_utc, const char* prefixo, const char* sufixo,
                           char* out, size_t size);

// MONOTONOUS CLOCK, in ms since the boot - to measure intervals, never stops
// show time. One for the entire system (there were eight local copies, half
// by the tick of FreeRTOS and half by esp_timer). Gives the iteration in ~49 days:
// always compare by difference (now - before), never by greater/smaller.
static inline uint32_t tab5_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
