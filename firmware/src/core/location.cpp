#include "core/location.h"
#include "core/nvs.h"
#include "core/ds_lock.h"
#include "util/log.h"
#include <math.h>
#include <string.h>

// RAM cache (the canonical name the UI reads). lat/lon are kept for the wear-threshold comparison so
// a write decision needs no NVS read. Guarded by s_lock: Core-0 fetch/hub tasks write, Core-1 UI reads.
static ds_lock_t s_lock;
static float     s_lat = 0, s_lon = 0;
static char      s_place[48] = "--";
static uint8_t   s_source = LOC_SRC_NONE;

void location_begin(void) {
  ds_lock_init(s_lock);
  float lat = 0, lon = 0; char tz[40] = "";
  bool have = nvs_get_location(&lat, &lon, tz, sizeof(tz));
  char place[48] = "";
  bool have_place = nvs_get_place(place, sizeof(place));
  ds_lock_take(s_lock);
  if (have) { s_lat = lat; s_lon = lon; }
  s_source = nvs_get_loc_src(LOC_SRC_NONE);
  if (have_place && place[0]) { strncpy(s_place, place, sizeof(s_place) - 1); s_place[sizeof(s_place) - 1] = 0; }
  ds_lock_give(s_lock);
}

void location_place(char* out, size_t cap) {
  if (!out || !cap) return;
  ds_lock_take(s_lock);
  strncpy(out, s_place, cap - 1); out[cap - 1] = 0;
  ds_lock_give(s_lock);
}

loc_source_t location_source(void) {
  ds_lock_take(s_lock);
  loc_source_t src = (loc_source_t)s_source;
  ds_lock_give(s_lock);
  return src;
}

// Common write path. `src` is the new source flag; the >0.01 deg threshold gates only the coord/tz
// NVS write (flash wear); the place + source persist on change regardless. Does NOT touch the clock.
static void location_set(float lat, float lon, const char* tz, const char* place, uint8_t src) {
  ds_lock_take(s_lock);
  if (src == LOC_SRC_IP && s_source == LOC_SRC_HUB) { ds_lock_give(s_lock); return; }  // hub wins; never downgraded by IP (under the lock => no TOCTOU)
  bool first = (s_source == LOC_SRC_NONE);
  bool moved = first || fabsf(s_lat - lat) > 0.01f || fabsf(s_lon - lon) > 0.01f;
  bool name_changed = place && place[0] && strncmp(s_place, place, sizeof(s_place)) != 0;
  bool src_changed  = (s_source != src);
  if (moved) { s_lat = lat; s_lon = lon; }
  if (name_changed) { strncpy(s_place, place, sizeof(s_place) - 1); s_place[sizeof(s_place) - 1] = 0; }
  s_source = src;
  ds_lock_give(s_lock);

  // NVS writes outside the cache copy's hot path is unnecessary here (IDF NVS is itself task-safe), but
  // keeping them after the cache update mirrors nvs.cpp's WiFi-blob pattern. Threshold-gated to spare flash.
  if (moved)        nvs_set_location(lat, lon, tz);
  if (name_changed) nvs_set_place(place);
  if (src_changed)  nvs_set_loc_src(src);
  if (moved || name_changed)
    LOGI("loc src=%u lat=%.3f lon=%.3f name=%s", (unsigned)src, lat, lon, place ? place : "");
}

void location_set_from_hub(float lat, float lon, const char* tz, const char* place) {
  location_set(lat, lon, tz, place, LOC_SRC_HUB);
}

void location_set_from_ip(float lat, float lon, const char* tz, const char* place) {
  location_set(lat, lon, tz, place, LOC_SRC_IP);   // location_set drops this if a hub fix already won
}
