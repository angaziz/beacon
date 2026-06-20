#include "core/nvs.h"
#include "core/ds_lock.h"
#include "util/log.h"
#include <Preferences.h>
#include <string.h>

static Preferences s_prefs;
static bool        s_open = false;

// Saved-WiFi working copy: persisted as one blob under "wnets", guarded by s_wifi_lock (cross-core:
// UI/portal mutate, Core-0 net reads). s_wifi_dirty signals net to rebuild the WiFiMulti AP set.
static wifi_list_t s_wifi;
static ds_lock_t   s_wifi_lock;
static volatile bool s_wifi_dirty = false;
static bool        s_wifi_open = false;

void nvs_begin(void) {
  s_open = s_prefs.begin("beacon", false);
  if (!s_open) LOGE("nvs begin failed (Preferences)");
  nvs_wifi_begin();
}

static void wifi_save_locked(void) { s_prefs.putBytes("wnets", &s_wifi, sizeof(s_wifi)); }

void nvs_wifi_begin(void) {
  if (!s_open) return;
  ds_lock_init(s_wifi_lock);
  memset(&s_wifi, 0, sizeof(s_wifi));
  if (!s_prefs.isKey("wnets")) {
    // One-time migration: seed from the legacy single creds, then DELETE them so a later empty list
    // can't resurrect a forgotten network. Writing "wnets" (even empty) marks migration done.
    if (s_prefs.isKey("wssid")) {
      String ss = s_prefs.getString("wssid", ""), pp = s_prefs.getString("wpass", "");
      if (ss.length()) wifi_list_add(&s_wifi, ss.c_str(), pp.c_str());
    }
    s_prefs.remove("wssid"); s_prefs.remove("wpass");   // always drop legacy keys (incl. a stray wpass)
    wifi_save_locked();
  } else if (s_prefs.getBytesLength("wnets") == sizeof(s_wifi)) {
    s_prefs.getBytes("wnets", &s_wifi, sizeof(s_wifi));
    if (s_wifi.count > WIFI_MAX_SAVED) memset(&s_wifi, 0, sizeof(s_wifi));   // corrupt guard
  }
  s_wifi_open = true;
}

bool nvs_has_wifi(void)  { return nvs_wifi_count() > 0; }
uint8_t nvs_wifi_count(void) {
  if (!s_wifi_open) return 0;
  ds_lock_take(s_wifi_lock); uint8_t c = s_wifi.count; ds_lock_give(s_wifi_lock); return c;
}
bool nvs_wifi_add(const char* ssid, const char* pass) {
  if (!s_wifi_open) return false;
  ds_lock_take(s_wifi_lock);
  bool ok = wifi_list_add(&s_wifi, ssid, pass);
  if (ok) { wifi_save_locked(); s_wifi_dirty = true; }
  ds_lock_give(s_wifi_lock);
  return ok;
}
bool nvs_wifi_forget(const char* ssid) {
  if (!s_wifi_open) return false;
  ds_lock_take(s_wifi_lock);
  bool ok = wifi_list_remove(&s_wifi, ssid);
  if (ok) { wifi_save_locked(); s_wifi_dirty = true; }
  ds_lock_give(s_wifi_lock);
  return ok;
}
bool nvs_wifi_get_ssid(uint8_t idx, char* out, size_t cap) {
  if (!s_wifi_open || !out || !cap) return false;
  ds_lock_take(s_wifi_lock);
  bool ok = idx < s_wifi.count;
  if (ok) { strncpy(out, s_wifi.e[idx].ssid, cap - 1); out[cap - 1] = 0; }
  ds_lock_give(s_wifi_lock);
  return ok;
}
void nvs_wifi_snapshot(wifi_list_t* out) {
  if (!out) return;
  ds_lock_take(s_wifi_lock); *out = s_wifi; ds_lock_give(s_wifi_lock);
}
bool nvs_wifi_dirty(void)       { return s_wifi_dirty; }
void nvs_wifi_clear_dirty(void) { s_wifi_dirty = false; }

uint8_t nvs_get_byte(const char* key, uint8_t def) { return s_open ? s_prefs.getUChar(key, def) : def; }
void    nvs_set_byte(const char* key, uint8_t v)   { if (s_open) s_prefs.putUChar(key, v); }

size_t nvs_get_bytes(const char* key, void* out, size_t cap) {
  if (!s_open || !out || !s_prefs.isKey(key)) return 0;
  if (s_prefs.getBytesLength(key) > cap) return 0;          // never overflow the caller's buffer
  return s_prefs.getBytes(key, out, cap);
}
bool nvs_set_bytes(const char* key, const void* data, size_t len) {
  if (!s_open) return false;
  return s_prefs.putBytes(key, data, len) == len;          // putBytes returns bytes written
}

uint8_t nvs_get_screen(uint8_t def)     { return s_open ? s_prefs.getUChar("screen", def) : def; }
void    nvs_set_screen(uint8_t v)       { if (s_open) s_prefs.putUChar("screen", v); }
uint8_t nvs_get_brightness(uint8_t def) { return s_open ? s_prefs.getUChar("bright", def) : def; }
void    nvs_set_brightness(uint8_t v)   { if (s_open) s_prefs.putUChar("bright", v); }
uint8_t nvs_get_theme(uint8_t def)      { return s_open ? s_prefs.getUChar("theme", def) : def; }
void    nvs_set_theme(uint8_t v)        { if (s_open) s_prefs.putUChar("theme", v); }
uint8_t nvs_get_dim_idx(uint8_t def)    { return s_open ? s_prefs.getUChar("dim_idx", def) : def; }
void    nvs_set_dim_idx(uint8_t v)      { if (s_open) s_prefs.putUChar("dim_idx", v); }
uint8_t nvs_get_sleep_idx(uint8_t def)  { return s_open ? s_prefs.getUChar("slp_idx", def) : def; }
void    nvs_set_sleep_idx(uint8_t v)    { if (s_open) s_prefs.putUChar("slp_idx", v); }

bool nvs_get_location(float* lat, float* lon, char* tz, size_t tz_cap) {
  if (!s_open || !s_prefs.isKey("lat")) return false;
  if (lat) *lat = s_prefs.getFloat("lat", 0);
  if (lon) *lon = s_prefs.getFloat("lon", 0);
  if (tz)  { String z = s_prefs.getString("tz", ""); strncpy(tz, z.c_str(), tz_cap - 1); tz[tz_cap - 1] = 0; }
  return true;
}

void nvs_set_location(float lat, float lon, const char* tz) {
  if (!s_open) return;
  s_prefs.putFloat("lat", lat);
  s_prefs.putFloat("lon", lon);
  s_prefs.putString("tz", tz ? tz : "");
}

bool nvs_get_place(char* out, size_t cap) {
  if (!s_open || !out || !cap || !s_prefs.isKey("place")) return false;
  String p = s_prefs.getString("place", "");
  strncpy(out, p.c_str(), cap - 1); out[cap - 1] = 0;
  return true;
}
void nvs_set_place(const char* name)   { if (s_open) s_prefs.putString("place", name ? name : ""); }
uint8_t nvs_get_loc_src(uint8_t def)   { return s_open ? s_prefs.getUChar("loc_src", def) : def; }
void    nvs_set_loc_src(uint8_t v)     { if (s_open) s_prefs.putUChar("loc_src", v); }
