#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "core/wifi_list.h"

// Persisted settings (FR-PLAT-3 / FR-SET-2 / FR-THEME-2 persistence) in NVS namespace "beacon".
// UI prefs survive reboot; WiFi creds are the device's own (never logged, tech.md §9). Getters take a
// default used when the key is unset. WiFi calibration is kept out of this partition via
// WiFi.persistent(false) (net.cpp) so the 20 KB nvs partition holds only these keys.
#ifdef __cplusplus
extern "C" {
#endif

void    nvs_begin(void);                 // open the namespace once at boot (before any get/set)

uint8_t nvs_get_byte(const char* key, uint8_t def);  void nvs_set_byte(const char* key, uint8_t v);  // generic (avoid IDF's nvs_get_u8)

uint8_t nvs_get_screen(uint8_t def);     void nvs_set_screen(uint8_t v);
uint8_t nvs_get_brightness(uint8_t def); void nvs_set_brightness(uint8_t v);
uint8_t nvs_get_theme(uint8_t def);      void nvs_set_theme(uint8_t v);
uint8_t nvs_get_dim_idx(uint8_t def);    void nvs_set_dim_idx(uint8_t v);
uint8_t nvs_get_sleep_idx(uint8_t def);  void nvs_set_sleep_idx(uint8_t v);

// Saved WiFi networks (canonical store; guarded by an internal mutex; net.cpp consumes it on Core-0).
// Mutations persist immediately and set a dirty flag the Core-0 net service polls. SSID getters never
// expose passwords. Call nvs_wifi_begin() once (from nvs_begin) to load + run the legacy migration.
void    nvs_wifi_begin(void);
bool    nvs_has_wifi(void);                          // true if at least one network is saved
bool    nvs_wifi_add(const char* ssid, const char* pass);   // dedup/update; false if list full + new
bool    nvs_wifi_forget(const char* ssid);                  // false if absent
uint8_t nvs_wifi_count(void);
bool    nvs_wifi_get_ssid(uint8_t idx, char* out, size_t cap);   // ssid only (never the password)
void    nvs_wifi_snapshot(wifi_list_t* out);         // by-value copy under lock (net builds WiFiMulti)
bool    nvs_wifi_dirty(void);                        // net polls; set on any add/forget
void    nvs_wifi_clear_dirty(void);

// Location override (FR-SET-4). false => caller uses DEFAULT_LOCATION.
bool    nvs_get_location(float* lat, float* lon, char* tz, size_t tz_cap);
void    nvs_set_location(float lat, float lon, const char* tz);

// Resolved place name + its source (issue #54); owned by core/location. Place getter returns false
// when unset (caller keeps "--"). Source is a loc_source_t byte (0 none / 1 ip / 2 hub).
bool    nvs_get_place(char* out, size_t cap);
void    nvs_set_place(const char* name);
uint8_t nvs_get_loc_src(uint8_t def);
void    nvs_set_loc_src(uint8_t v);

#ifdef __cplusplus
}
#endif
