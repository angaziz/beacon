#pragma once
#include <stdint.h>
#include <stddef.h>

// Canonical place/location owner (issue #54). Resolves the time-screen place name from one of three
// sources, in precedence order: hub CoreLocation (best) => cached NVS (survives reboot) => IP
// geolocation (fetch/geoip, coarsest). A hub-sourced fix is never overwritten by the IP path and
// never auto-reverts (stationary-device assumption). Holds a small RAM cache guarded by a ds_lock so
// the Core-0 fetch/hub tasks can write while the Core-1 UI reads the name. Coordinates+tz persist via
// nvs_set_location() (weather + boot tz restore read them); the place name + source via new NVS keys.
//
// tz application to the clock is the CALLER's job (timekeep_set_tz, OUTSIDE this module's lock):
// setenv/tzset is not thread-safe and is already called unlocked from fetch_geoip; keeping it out of
// the location critical section avoids a new lock-coupling/deadlock surface.
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LOC_SRC_NONE = 0, LOC_SRC_IP = 1, LOC_SRC_HUB = 2 } loc_source_t;

void         location_begin(void);                  // load cache from NVS at boot (after nvs_begin)
void         location_place(char* out, size_t cap); // place name; "--" until known (locked copy)
loc_source_t location_source(void);                 // current best source (locked read)

// Persist + cache a fix. Coordinates+tz write to NVS only past the >0.01 deg wear threshold; the place
// name writes only when the string changes; the source flag always updates (so an IP=>HUB promotion
// sticks even when coords are unchanged). set_from_ip() is a no-op once source==LOC_SRC_HUB.
void location_set_from_hub(float lat, float lon, const char* tz, const char* place);
void location_set_from_ip (float lat, float lon, const char* tz, const char* place);

#ifdef __cplusplus
}
#endif
