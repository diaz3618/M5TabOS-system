// RTC (RX8130) - has its own battery and keeps time across boots.
// It used to be merely ENABLED (profile.rtc=true), never read or set by anyone.
// M5.begin() already calls M5.Rtc.setSystemTimeFromRtc() itself (see
// M5Unified.cpp), so ANY CODE that needs today's date only has to call libc's
// time()/gmtime(). That performs no I2C read, which is why there is no
// tab5_rtc_read() here: it is unnecessary.
//
// WRITING WAS THE MISSING PIECE. Until someone sets the RX8130 at least once,
// it keeps a meaningless factory time. The battery preserves what was written;
// it cannot invent the correct time. See services/tab5_time.hpp for WHO decides
// when to set it and which source to use.
#pragma once

#include <stdbool.h>
#include <time.h>

// Writes the RTC AND the system clock (settimeofday) IN THE SAME CALL. Without
// the second step, time()/gmtime() would return the right value only after the
// NEXT boot (because M5.begin() reads the RTC then). Doing both means callers
// see the correct time immediately, in this same session.
//
// UI TASK ONLY - the RX8130 is on internal I2C (RTC_Base uses In_I2C by
// default, the same bus as touch/IMU/PMIC), under the rule documented in
// tab5_power.hpp. epoch_utc is UTC seconds since 1970.
bool tab5_rtc_set(time_t epoch_utc);

// Does the RX8130 respond on the bus? (This says only that the chip exists and
// is accessible, not that its stored time is CORRECT.)
bool tab5_rtc_available(void);
