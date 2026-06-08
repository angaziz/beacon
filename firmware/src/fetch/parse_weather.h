#pragma once
#include <stddef.h>
#include "core/records.h"   // weather_rec_t, data_err_t (via screen_state.h)

// Parse an Open-Meteo current-weather JSON into a weather_rec_t's value fields (temp/humidity/wmo).
// Pure + host-testable; does NOT touch out->hdr (the caller stamps state/time). Returns ERR_PARSE on
// malformed/missing fields, ERR_NONE on success.
#ifdef __cplusplus
extern "C" {
#endif
data_err_t parse_weather(const char* json, size_t len, weather_rec_t* out);
#ifdef __cplusplus
}
#endif
