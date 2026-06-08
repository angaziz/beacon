#include "fetch/geoip.h"
#include "fetch/parse_geoip.h"
#include "core/net.h"
#include "core/nvs.h"
#include "core/timekeep.h"
#include "util/log.h"
#include <math.h>
#include <string.h>
#include <strings.h>   // strcasecmp

static char s_buf[4096];   // holds ipwho.is (~1.2 KB) then the BigDataCloud reverse-geocode (~2.5 KB)
static char s_city[48] = "--";

const char* geoip_city(void) { return s_city; }

data_err_t fetch_geoip(void) {
  const char* hk[] = { "User-Agent" };
  const char* hv[] = { "Mozilla/5.0 (compatible; Beacon/1.0)" };   // some IP APIs reject an empty UA
  int status = 0;
  data_err_t e = net_https_get("ipwho.is", "/", hk, hv, 1, s_buf, sizeof(s_buf), &status);
  if (e != ERR_NONE) return e;

  // area = ipwho.is granular locality (e.g. "Suka Asih"); region (province) is intentionally ignored.
  float lat = 0, lon = 0; char tz[40] = ""; char area[40] = "";
  if (parse_geoip(s_buf, strlen(s_buf), &lat, &lon, tz, sizeof(tz),
                  area, sizeof(area), nullptr, 0) != ERR_NONE) return ERR_PARSE;

  // Reverse-geocode the coords to the recognizable city/kota name. Best-effort: keep just the area on failure.
  char city[40] = "";
  char path[112];
  snprintf(path, sizeof(path),
           "/data/reverse-geocode-client?latitude=%.4f&longitude=%.4f&localityLanguage=en", lat, lon);
  if (net_https_get("api.bigdatacloud.net", path, hk, hv, 1, s_buf, sizeof(s_buf), &status) == ERR_NONE)
    parse_bdc_city(s_buf, strlen(s_buf), city, sizeof(city));

  // "Area, City" => e.g. "Suka Asih, Bandung". Never the province; collapse "X, X" duplicates.
  if (area[0] && city[0] && strcasecmp(area, city) != 0) snprintf(s_city, sizeof(s_city), "%s, %s", area, city);
  else if (city[0]) snprintf(s_city, sizeof(s_city), "%s", city);
  else if (area[0]) snprintf(s_city, sizeof(s_city), "%s", area);

  // Persist only on a meaningful move (avoid NVS wear). The weather fetcher reads nvs_get_location().
  float olat = 0, olon = 0; char otz[40] = "";
  bool have = nvs_get_location(&olat, &olon, otz, sizeof(otz));
  if (!have || fabsf(olat - lat) > 0.01f || fabsf(olon - lon) > 0.01f) {
    nvs_set_location(lat, lon, tz);
    LOGI("geoip lat=%.3f lon=%.3f tz=%s", lat, lon, tz);
  }
  if (tz[0] && timekeep_tz_supported(tz)) timekeep_set_tz(tz);   // keep current tz if provider's is unmapped
  return ERR_NONE;
}
