#pragma once
#include <stddef.h>
#include "core/screen_state.h"   // data_err_t

// Parse an ipwho.is response into lat/lon + IANA timezone id. Pure + host-testable. Returns ERR_PARSE
// on malformed input or success:false; ERR_NONE with the fields filled otherwise. tz may be empty if
// the provider omits it (caller keeps its current tz).
#ifdef __cplusplus
extern "C" {
#endif
// tz/city/region are optional (pass NULL/0 to skip); each is left empty when the provider omits it.
data_err_t parse_geoip(const char* json, size_t len, float* lat, float* lon,
                       char* tz, size_t tz_cap, char* city, size_t city_cap,
                       char* region, size_t region_cap);

// BigDataCloud reverse-geocode: pick the city/kota level (admin entry whose description begins
// "city ...") from localityInfo.administrative => a recognizable name (e.g. "San Francisco") vs ipwho.is's
// over-granular locality. ERR_PARSE if no city-level entry is present.
data_err_t parse_bdc_city(const char* json, size_t len, char* city, size_t city_cap);
#ifdef __cplusplus
}
#endif
