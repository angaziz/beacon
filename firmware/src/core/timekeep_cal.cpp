#include "core/timekeep_cal.h"

// UTC broken-down -> epoch (days-from-civil, Howard Hinnant). newlib lacks timegm; this is
// TZ-independent so it correctly treats the RTC fields as UTC. y/m/d are calendar values (m 1..12).
time_t utc_to_epoch(int y, unsigned m, unsigned d, unsigned hh, unsigned mm, unsigned ss) {
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = (long)era * 146097 + (long)doe - 719468;   // days since 1970-01-01
  return (time_t)(days * 86400L + (long)hh * 3600 + (long)mm * 60 + (long)ss);
}
