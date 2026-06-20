#pragma once
#include <time.h>

// Pure UTC calendar conversion (Howard Hinnant days-from-civil). Extracted so the host-native test env
// can cover the algorithm independently of Arduino/RTC/NTP. y/m/d are calendar values (m 1..12).
#ifdef __cplusplus
extern "C" {
#endif
time_t utc_to_epoch(int y, unsigned m, unsigned d, unsigned hh, unsigned mm, unsigned ss);
#ifdef __cplusplus
}
#endif
