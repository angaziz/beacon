#include "fetch/weather.h"
#include "fetch/parse_weather.h"
#include "core/net.h"
#include "core/nvs.h"
#include "core/datastore.h"
#include "core/timekeep.h"
#include "config/location.h"
#include "util/log.h"
#include <stdio.h>
#include <string.h>

static char s_buf[1024];   // Open-Meteo current payload ~420 B

data_err_t fetch_weather(void) {
  float lat = DEFAULT_LOCATION.lat, lon = DEFAULT_LOCATION.lon;
  char tz[40];
  nvs_get_location(&lat, &lon, tz, sizeof(tz));   // NVS override if set; else defaults retained

  char path[160];
  snprintf(path, sizeof(path),
           "/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,weather_code", lat, lon);

  int status = 0;
  data_err_t e = net_https_get("api.open-meteo.com", path, nullptr, nullptr, 0, s_buf, sizeof(s_buf), &status);
  if (e != ERR_NONE) { ds_set_state_weather(e == ERR_NO_ROUTE ? ST_OFFLINE : ST_ERROR, e); return e; }

  weather_rec_t w; memset(&w, 0, sizeof(w));
  e = parse_weather(s_buf, strlen(s_buf), &w);
  if (e != ERR_NONE) { ds_set_state_weather(ST_ERROR, e); return e; }

  w.hdr.last_updated = (uint32_t)timekeep_now();
  ds_set_weather(&w);   // forces ST_LIVE / ERR_NONE
  return ERR_NONE;
}
