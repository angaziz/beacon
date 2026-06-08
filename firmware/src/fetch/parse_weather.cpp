#include "fetch/parse_weather.h"
#include <ArduinoJson.h>

// Open-Meteo: {"current":{"temperature_2m":31.8,"relative_humidity_2m":57,"weather_code":2}, ...}
data_err_t parse_weather(const char* json, size_t len, weather_rec_t* out) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return ERR_PARSE;
  JsonObjectConst cur = doc["current"];
  if (cur.isNull() || cur["temperature_2m"].isNull()) return ERR_PARSE;
  out->temp_c       = cur["temperature_2m"].as<float>();
  out->humidity_pct = cur["relative_humidity_2m"].as<float>();
  out->wmo_code     = cur["weather_code"].as<uint16_t>();
  return ERR_NONE;
}
