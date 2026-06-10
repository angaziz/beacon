#pragma once
#include <stdint.h>
#include "config/tickers.h"

// Per-source stale thresholds (tech.md §6 cadence table). Finance is per-ticker.
#define WEATHER_STALE_S    1800u   // 30 min
#define USAGE_STALE_S       300u   // 5 min / hub-offline
#define BUDDY_STALE_S       300u

static inline uint32_t finance_stale_s(uint8_t idx) {
  return (idx < DEFAULT_TICKERS_COUNT) ? DEFAULT_TICKERS[idx].stale_s : 0u;
}
