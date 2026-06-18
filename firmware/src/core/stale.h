#pragma once
#include <stdint.h>
#include "config/ticker_table.h"

// Per-source stale thresholds (tech.md §6 cadence table). Finance is per-ticker.
#define WEATHER_STALE_S    1800u   // 30 min
#define USAGE_STALE_S       300u   // 5 min / hub-offline
#define BUDDY_STALE_S       300u

// Read the per-row stale window from the RUNTIME table (hub-pushed or default), not DEFAULT_TICKERS:
// a config with rows beyond the default count would otherwise get stale_s=0 and be marked ST_STALE right
// after a successful fetch (#92).
static inline uint32_t finance_stale_s(uint8_t idx) {
  ticker_runtime_t t;
  return ticker_table_get((int)idx, &t) ? t.stale_s : 0u;
}
