#include "fetch/geoip.h"
#include "fetch/parse_geoip.h"
#include "core/net.h"
#include "core/nvs.h"
#include "core/location.h"
#include "core/timekeep.h"
#include "core/fetch_task.h"
#include "util/log.h"
#include <string.h>
#include <strings.h>   // strcasecmp

// The place name now lives in core/location (issue #54); this stays the Settings/Home accessor and
// returns whichever source won (hub > cached > ip). A function-local static backs the const char*.
const char* geoip_city(void) {
  static char s_city[48];
  location_place(s_city, sizeof(s_city));
  return s_city;
}

data_err_t fetch_geoip(void) {
  if (location_source() == LOC_SRC_HUB) return ERR_NONE;   // hub CoreLocation wins; skip the IP path entirely

  const char* hk[] = { "User-Agent" };
  const char* hv[] = { "Mozilla/5.0 (compatible; Beacon/1.0)" };   // some IP APIs reject an empty UA
  int status = 0;
  data_err_t e = net_https_get("ipwho.is", "/", hk, hv, 1, fetch_scratch(), fetch_scratch_cap(), &status);
  if (e != ERR_NONE) return e;

  // area = ipwho.is granular locality (e.g. "Mission"); region (province) is intentionally ignored.
  float lat = 0, lon = 0; char tz[40] = ""; char area[40] = "";
  if (parse_geoip(fetch_scratch(), strlen(fetch_scratch()), &lat, &lon, tz, sizeof(tz),
                  area, sizeof(area), nullptr, 0) != ERR_NONE) return ERR_PARSE;

  // Reverse-geocode the coords to the recognizable city/kota name. Best-effort: keep just the area on failure.
  char city[40] = "";
  char path[112];
  snprintf(path, sizeof(path),
           "/data/reverse-geocode-client?latitude=%.4f&longitude=%.4f&localityLanguage=en", lat, lon);
  data_err_t rgeo = net_https_get("api.bigdatacloud.net", path, hk, hv, 1, fetch_scratch(), fetch_scratch_cap(), &status);
  if (rgeo == ERR_NONE)
    parse_bdc_city(fetch_scratch(), strlen(fetch_scratch()), city, sizeof(city));
  else
    LOGW("reverse geocode failed err=%d; using area only", (int)rgeo);

  // "Area, City" => e.g. "Mission, San Francisco". Never the province; collapse "X, X" duplicates.
  char name[48] = "";
  if (area[0] && city[0] && strcasecmp(area, city) != 0) snprintf(name, sizeof(name), "%s, %s", area, city);
  else if (city[0]) snprintf(name, sizeof(name), "%s", city);
  else if (area[0]) snprintf(name, sizeof(name), "%s", area);

  // Persist via core/location (threshold-gated NVS write; the weather fetcher reads nvs_get_location()).
  location_set_from_ip(lat, lon, tz, name);
  if (tz[0] && timekeep_tz_supported(tz)) timekeep_set_tz(tz);   // keep current tz if provider's is unmapped
  return ERR_NONE;
}
