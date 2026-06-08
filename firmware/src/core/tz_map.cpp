#include "core/tz_map.h"
#include <string.h>

// Supported IANA -> POSIX TZ. DST rules embedded where applicable; the default is Asia/Jakarta.
typedef struct { const char* iana; const char* posix; } tz_pair_t;
static const tz_pair_t TZ_TABLE[] = {
  {"Asia/Jakarta",        "WIB-7"},
  {"Asia/Makassar",       "WITA-8"},
  {"Asia/Jayapura",       "WIT-9"},
  {"Asia/Singapore",      "<+08>-8"},
  {"Asia/Shanghai",       "CST-8"},
  {"Asia/Tokyo",          "JST-9"},
  {"Asia/Kolkata",        "IST-5:30"},
  {"Australia/Sydney",    "AEST-10AEDT,M10.1.0,M4.1.0/3"},
  {"Europe/London",       "GMT0BST,M3.5.0/1,M10.5.0"},
  {"Europe/Berlin",       "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"America/New_York",    "EST5EDT,M3.2.0,M11.1.0"},
  {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
  {"UTC",                 "UTC0"},
};

const char* tz_iana_to_posix(const char* iana) {
  if (!iana) return 0;
  for (unsigned i = 0; i < sizeof(TZ_TABLE) / sizeof(TZ_TABLE[0]); i++)
    if (strcmp(TZ_TABLE[i].iana, iana) == 0) return TZ_TABLE[i].posix;
  return 0;
}
