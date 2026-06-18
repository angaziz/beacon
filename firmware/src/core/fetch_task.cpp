#include "core/fetch_task.h"
#include "core/datastore.h"
#include "core/net.h"
#include "core/timekeep.h"
#include "fetch/weather.h"
#include "fetch/finance.h"
#include "fetch/geoip.h"
#include "config/tickers.h"
#include "util/log.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

#define WEATHER_CADENCE_S 600u    // 10 min (tech.md §6)
#define RETRY_S            60u    // flat retry after a failed fetch (backoff added only if 429s appear)

// Source slots: index 0 = weather, 1..N = ticker[idx-1]. next_due holds the epoch each is due.
#define SRC_WEATHER 0
static uint32_t s_next_due[1 + MAX_TICKERS];

// One shared scratch for every fetch body (#65 M6); 8KB fits the largest (Yahoo chart ~unfiltered).
static char s_scratch[8192];
char*  fetch_scratch(void) { return s_scratch; }
size_t fetch_scratch_cap(void) { return sizeof(s_scratch); }

static uint32_t cadence_of(int slot) {
  return (slot == SRC_WEATHER) ? WEATHER_CADENCE_S : DEFAULT_TICKERS[slot - 1].cadence_s;
}

// Host each slot fetches from -- lets the scheduler drain same-host due slots back-to-back so one TLS
// handshake serves the whole sweep (#61). Must match the host strings passed to net_https_get in the
// fetch modules; a mismatch only loses the reuse (falls back to oldest-due), never correctness.
static const char* slot_host(int slot) {
  if (slot == SRC_WEATHER) return "api.open-meteo.com";
  switch (DEFAULT_TICKERS[slot - 1].source) {
    case SRC_YAHOO:       return "query1.finance.yahoo.com";
    case SRC_BINANCE:     return "data-api.binance.vision";
    default:              return "";
  }
}

static data_err_t run_slot(int slot) {
  return (slot == SRC_WEATHER) ? fetch_weather() : fetch_finance((uint8_t)(slot - 1));
}

// Flip all device-plane records to ST_OFFLINE when the link drops (don't fetch while down).
static void mark_offline(void) {
  ds_set_state_weather(ST_OFFLINE, ERR_NO_ROUTE);
  for (uint8_t i = 0; i < ds_get_finance_count(); i++) ds_set_state_finance(i, ST_OFFLINE, ERR_NO_ROUTE);
}

static void fetch_task(void*) {
  const int slots = 1 + DEFAULT_TICKERS_COUNT;
  for (int i = 0; i < slots; i++) s_next_due[i] = 0;   // all due at first connect
  bool was_up = false;
  bool dn_marked = false;    // device-plane already flipped offline for the current down stretch
  bool geo_pending = true;   // resolve location once per (re)connect, before weather
  int  hk = 0;

  for (;;) {
    net_service();   // WiFiMulti: apply saved-list changes + gated reconnect (blocks only while down)
    uint32_t now = (uint32_t)timekeep_now();
    ds_tick_staleness(now);

    bool up = net_is_up();
    if (!up) {
      // Mark offline on the FIRST observed down too (boot with bad creds / WiFi disabled), not only on
      // an up->down transition; net_service blocks on run() so a good-creds boot is already up here.
      if (!dn_marked) { LOGW("net down; device-plane offline"); mark_offline(); dn_marked = true; }
      was_up = false;
    } else {
      if (!was_up) { for (int i = 0; i < slots; i++) s_next_due[i] = now; geo_pending = true; }  // reconnect => refetch soon
      was_up = true;
      dn_marked = false;

      if (timekeep_has_time()) {                 // hold fetches until the clock is real (consistent stamps)
        if (geo_pending) {
          fetch_geoip();                         // resolve lat/lon + tz before weather; best-effort
          geo_pending = false;                   // weather (next iterations) then uses the resolved coords
        } else {
          // Drain a due slot on the already-open host first so a sweep reuses one kept-alive socket
          // (#61); otherwise take the oldest-due slot.
          int pick = -1; uint32_t oldest = now + 1;
          const char* openh = net_open_host();
          if (openh[0])
            for (int i = 0; i < slots; i++)
              if (s_next_due[i] <= now && strcmp(slot_host(i), openh) == 0) { pick = i; break; }
          if (pick < 0)
            for (int i = 0; i < slots; i++)
              if (s_next_due[i] <= now && s_next_due[i] < oldest) { oldest = s_next_due[i]; pick = i; }
          if (pick >= 0) {
            data_err_t e = run_slot(pick);
            now = (uint32_t)timekeep_now();       // fetch blocks; re-read clock for scheduling
            s_next_due[pick] = now + (e == ERR_NONE ? cadence_of(pick) : RETRY_S);
          }
        }
      }
    }

    net_close_idle();   // drop the kept-alive HTTPS socket once a sweep goes idle (#61)

    if (++hk % 10 == 0)
      LOGI("heap: int_free=%u int_min=%u psram_free=%u up=%d time=%d",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), up, timekeep_has_time());

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void fetch_task_start(void) {
  // 8 KB stack headroom for the TLS handshake + HTTPClient call chain (payload buffers + ArduinoJson
  // docs live in .bss/heap, not here).
  xTaskCreatePinnedToCore(fetch_task, "fetch", 8192, nullptr, 1, nullptr, 0);   // Core 0
}
