#include "fetch/parse_geoip.h"
#include <ArduinoJson.h>
#include <string.h>
#include <strings.h>   // strncasecmp

// ipwho.is: {"success":true,"latitude":-6.93,"longitude":107.59,...,"timezone":{"id":"Asia/Jakarta",...}}
static void copy_field(JsonVariantConst v, char* out, size_t cap) {
  if (!out || !cap) return;
  const char* s = v | "";
  strncpy(out, s, cap - 1); out[cap - 1] = 0;
}

data_err_t parse_geoip(const char* json, size_t len, float* lat, float* lon,
                       char* tz, size_t tz_cap, char* city, size_t city_cap,
                       char* region, size_t region_cap) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return ERR_PARSE;
  if (doc["success"].is<bool>() && !doc["success"].as<bool>()) return ERR_PARSE;   // {"success":false,...}
  JsonVariantConst la = doc["latitude"], lo = doc["longitude"];
  if (la.isNull() || lo.isNull()) return ERR_PARSE;
  if (lat) *lat = la.as<float>();
  if (lon) *lon = lo.as<float>();
  copy_field(doc["timezone"]["id"], tz, tz_cap);
  copy_field(doc["city"], city, city_cap);
  copy_field(doc["region"], region, region_cap);
  return ERR_NONE;
}

data_err_t parse_bdc_city(const char* json, size_t len, char* city, size_t city_cap) {
  // Filter to just the administrative array's name/description (response carries a large informative
  // array we don't need); element [0] in a filter applies to every array element.
  JsonDocument filter;
  filter["localityInfo"]["administrative"][0]["name"] = true;
  filter["localityInfo"]["administrative"][0]["description"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, json, len, DeserializationOption::Filter(filter))) return ERR_PARSE;
  JsonArrayConst adm = doc["localityInfo"]["administrative"];
  if (adm.isNull()) return ERR_PARSE;
  for (JsonObjectConst e : adm) {
    const char* d = e["description"] | "";
    if (strncasecmp(d, "city", 4) == 0) {   // the kota/city level (vs province/district/island)
      copy_field(e["name"], city, city_cap);
      if (city && city[0]) return ERR_NONE;
    }
  }
  return ERR_PARSE;
}
