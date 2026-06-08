#pragma once
#include "core/screen_state.h"   // data_err_t

// IP-based geolocation (no GPS on this board): resolve approximate lat/lon + timezone from the public
// IP via ipwho.is, persist to NVS (so the weather fetcher picks it up and offline boots keep it), and
// apply the timezone to the clock. Runs once per (re)connect from the Core-0 fetch task, so the device
// follows you when you change networks. City-level accuracy (ISP egress).
#ifdef __cplusplus
extern "C" {
#endif
data_err_t fetch_geoip(void);
const char* geoip_city(void);   // last resolved city ("--" until known); for the Settings/Home display
#ifdef __cplusplus
}
#endif
