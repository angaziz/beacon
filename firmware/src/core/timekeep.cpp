#include "core/timekeep.h"
#include "core/timekeep_cal.h"
#include "core/tz_map.h"
#include "core/nvs.h"
#include "config/location.h"
#include "util/log.h"
#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include <SensorPCF85063.hpp>

static SensorPCF85063 s_rtc;
static volatile bool s_rtc_ok      = false;
static volatile bool s_have_time   = false;
// SNTP fires its callback on the tcpip task; the RTC lives on the shared I2C bus that Core-1 also
// drives for touch/PMU. Writing the RTC from the callback would race those reads (no bus mutex), so
// the callback only stages the sync and timekeep_service() (pumped from the Core-1 loop) performs the
// actual I2C write, serialized with touch. s_active_posix keeps NTP from clobbering a user-set TZ.
static volatile bool s_rtc_write_pending = false;
static time_t        s_rtc_write_epoch   = 0;
static char          s_active_posix[40]  = "UTC0";

// Apply a known IANA zone. `boot` forces a value (UTC if unknown) so the clock always has a zone;
// at runtime an unknown id is ignored (keeps the current zone) so a geoip miss never breaks the clock.
static void apply_tz(const char* iana, bool boot) {
  const char* posix = tz_iana_to_posix(iana);
  if (!posix) {
    LOGW("tz unknown '%s'%s", iana ? iana : "(null)", boot ? " => UTC" : " => keeping current");
    if (!boot) return;
    posix = "UTC0";
  }
  strncpy(s_active_posix, posix, sizeof(s_active_posix) - 1);
  s_active_posix[sizeof(s_active_posix) - 1] = 0;
  setenv("TZ", s_active_posix, 1);
  tzset();
}

// utc_to_epoch lives in timekeep_cal.cpp (host-testable, shared here).

// RTC holds UTC; convert its broken-down fields to an epoch (no TZ applied).
static time_t rtc_epoch(const RTC_DateTime& d) {
  return utc_to_epoch(d.getYear(), d.getMonth(), d.getDay(), d.getHour(), d.getMinute(), d.getSecond());
}

// SNTP set the system clock. Stage the RTC mirror for the Core-1 loop (no I2C from this task context).
static void on_sntp_sync(struct timeval* tv) {
  s_rtc_write_epoch   = tv ? tv->tv_sec : time(nullptr);
  s_rtc_write_pending = true;
  s_have_time = true;
  LOGI("ntp sync epoch=%ld; rtc write staged", (long)s_rtc_write_epoch);
}

void timekeep_init(void) {
  // Prefer the tz geoip last saved (nvs already open) so an offline reboot keeps the resolved zone;
  // fall back to the default when unset or unmapped. localtime is correct the moment time is known.
  float lat, lon; char tz[40];
  bool have = nvs_get_location(&lat, &lon, tz, sizeof(tz));
  apply_tz(have && tz[0] && timekeep_tz_supported(tz) ? tz : DEFAULT_LOCATION.tz_id, true);
  s_rtc_ok = s_rtc.begin(Wire);              // shared bus, already begun by power_begin()
  if (!s_rtc_ok) { LOGE("rtc PCF85063 init failed"); return; }
  RTC_DateTime d = s_rtc.getDateTime();
  if (d.getYear() >= 2024) {                 // a fresh/unset PCF85063 reads year 2000 => treat as invalid
    struct timeval tv = { .tv_sec = rtc_epoch(d), .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    s_have_time = true;
    LOGI("rtc time loaded epoch=%ld year=%u", (long)tv.tv_sec, d.getYear());
  } else {
    LOGW("rtc time invalid (year=%u); awaiting NTP", d.getYear());
  }
}

void timekeep_set_tz(const char* iana) { apply_tz(iana, false); }
bool timekeep_tz_supported(const char* iana) { return tz_iana_to_posix(iana) != 0; }

void timekeep_start_ntp(void) {
  sntp_set_time_sync_notification_cb(on_sntp_sync);  // must precede SNTP init below
  // Use the ACTIVE TZ (a user Settings choice may already have replaced the default) so configTzTime,
  // which also sets the TZ env, does not clobber it back to Asia/Jakarta.
  configTzTime(s_active_posix, DEFAULT_LOCATION.ntp_server, "time.google.com", "time.cloudflare.com");
  LOGI("ntp started server=%s tz=%s", DEFAULT_LOCATION.ntp_server, s_active_posix);
}

// Pump from the Core-1 loop: perform any staged RTC write here so it is serialized with touch/PMU on
// the shared I2C bus (the SNTP callback only stages it).
void timekeep_service(void) {
  if (!s_rtc_write_pending) return;
  s_rtc_write_pending = false;
  if (!s_rtc_ok) return;
  struct tm utc; time_t e = s_rtc_write_epoch; gmtime_r(&e, &utc);
  s_rtc.setDateTime(RTC_DateTime(utc));
  LOGI("rtc persisted epoch=%ld", (long)e);
}

bool timekeep_has_time(void) { return s_have_time; }

time_t timekeep_now(void) { return time(nullptr); }

// Single UI clock (declared in ui/screen.h). Same epoch the fetchers stamp => consistent staleness.
uint32_t now_s(void) { return (uint32_t)time(nullptr); }

// Monotonic uptime (declared in ui/screen.h). Used for prompt lifecycle timeouts so an NTP/RTC jump
// to the wall clock cannot expire a prompt instantly.
uint32_t uptime_s(void) { return (uint32_t)(millis() / 1000); }

void timekeep_localtime(struct tm* out) {
  time_t now = time(nullptr);
  localtime_r(&now, out);
}
