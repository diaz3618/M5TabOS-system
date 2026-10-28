#include "tab5_rtc.hpp"
#include <M5Unified.h>
#include <sys/time.h>

bool tab5_rtc_available(void) { return M5.Rtc.isEnabled(); }

bool tab5_rtc_set(time_t epoch_utc)
{
  if (!M5.Rtc.isEnabled()) { return false; }

  struct tm t = { };
  gmtime_r(&epoch_utc, &t);

  // RTC_Class::setDateTime(const tm*) is the most direct overload and returns
  // void. The class handles write failures internally, so there is nothing to
  // check here.
  M5.Rtc.setDateTime(&t);

  // Set the system clock too, in the same call; see the note in the header.
  struct timeval tv = { };
  tv.tv_sec  = epoch_utc;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);

  return true;
}
