#pragma once
#include <stdint.h>

// FROZEN schema (FR-STATE-0). Values editable; NVS override in chunk D.
typedef struct {
  float       lat, lon;
  const char* units;       // "metric"
  const char* tz_id;       // IANA, e.g. "Asia/Jakarta"
  const char* ntp_server;
} location_cfg_t;

static const location_cfg_t DEFAULT_LOCATION = { -6.2f, 106.8f, "metric", "Asia/Jakarta", "pool.ntp.org" };

// WMO weather code -> short label + icon id (fixed table). Open-Meteo current.weather_code buckets.
typedef struct { uint16_t code; const char* label; const char* icon; } wmo_entry_t;
static const wmo_entry_t WMO_MAP[] = {
  {0,  "Clear",          "clear"},
  {1,  "Mainly clear",   "clear"},
  {2,  "Partly cloudy",  "partly"},
  {3,  "Overcast",       "cloud"},
  {45, "Fog",            "fog"},
  {48, "Rime fog",       "fog"},
  {51, "Light drizzle",  "drizzle"},
  {53, "Drizzle",        "drizzle"},
  {55, "Dense drizzle",  "drizzle"},
  {61, "Light rain",     "rain"},
  {63, "Rain",           "rain"},
  {65, "Heavy rain",     "rain"},
  {71, "Light snow",     "snow"},
  {73, "Snow",           "snow"},
  {75, "Heavy snow",     "snow"},
  {80, "Showers",        "showers"},
  {81, "Showers",        "showers"},
  {82, "Violent showers","showers"},
  {95, "Thunderstorm",   "storm"},
  {96, "Storm + hail",   "storm"},
  {99, "Storm + hail",   "storm"},
};

#define WMO_MAP_COUNT ((uint16_t)(sizeof(WMO_MAP) / sizeof(WMO_MAP[0])))
