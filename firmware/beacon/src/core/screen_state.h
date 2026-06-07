#pragma once
#include <stdint.h>

typedef enum {
  ST_LOADING,     // first fetch in flight, no value yet
  ST_LIVE,        // fresh value
  ST_STALE,       // last value older than the source's stale_s
  ST_OFFLINE,     // transport down (WiFi for device-plane)
  ST_ERROR,       // fetch failed / rate-limited (see data_err_t)
  ST_HUB_OFFLINE, // BLE hub disconnected (usage/buddy only)
  ST_RECONNECTING // transport re-establishing; actions stay disabled until confirmed (DESIGN.md)
} screen_state_t;

typedef enum {
  ERR_NONE, ERR_TIMEOUT, ERR_HTTP, ERR_RATE_LIMITED, ERR_PARSE, ERR_NO_ROUTE
} data_err_t;

// State priority (frozen rule). The staleness sweep (datastore) may only promote
// ST_LIVE => ST_STALE. It must NEVER overwrite ST_OFFLINE / ST_ERROR / ST_HUB_OFFLINE,
// which are set explicitly by fetchers/transport and cleared only by a successful update
// (ST_LIVE) or an explicit transition. Display precedence high=>low:
// ST_ERROR / ST_OFFLINE / ST_HUB_OFFLINE > ST_STALE > ST_LIVE > ST_LOADING.
