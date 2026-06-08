#pragma once
#include <time.h>
#include <stdbool.h>
#include <stdint.h>

// Time service (FR-PLAT-8): PCF85063 RTC (offline hold) + NTP/SNTP (sync) + POSIX TZ from config.
// RTC stores UTC; localtime is derived via the active TZ. The Core-0 fetch task and the UI read
// timekeep_now()/timekeep_localtime(); no other module touches the clock. Until timekeep_has_time(),
// the UI shows "--:--" and fetchers hold ST_LOADING (avoids stamping values against a pre-sync clock).
#ifdef __cplusplus
extern "C" {
#endif

void   timekeep_init(void);               // bring up RTC on the shared Wire bus; seed system clock if RTC valid; apply default TZ
void   timekeep_set_tz(const char* iana); // remap TZ at runtime; keeps the current zone if id unknown
bool   timekeep_tz_supported(const char* iana); // true if the IANA id maps to a known POSIX zone
void   timekeep_start_ntp(void);          // start SNTP (call once WiFi is up); stages an RTC mirror on sync
void   timekeep_service(void);            // pump from the Core-1 loop; performs the staged RTC write (I2C)
bool   timekeep_has_time(void);           // true once RTC-valid or NTP-synced
time_t timekeep_now(void);                // UTC epoch seconds (system time)
void   timekeep_localtime(struct tm* out);// broken-down local time per the active TZ

#ifdef __cplusplus
}
#endif
