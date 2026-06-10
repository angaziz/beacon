#pragma once
#include <stdint.h>

// Shared dim/sleep timeout presets (FR-PLAT-7 / FR-SET-5). ms == 0 => "Never".
// Indices are persisted in NVS (nvs_get/set_dim_idx, _sleep_idx). Append-only:
// never reorder, or stored indices shift meaning.
typedef struct { const char* label; uint32_t ms; } duration_opt_t;

static const duration_opt_t DURATIONS[] = {
  { "15 sec", 15000  },
  { "30 sec", 30000  },
  { "1 min",  60000  },
  { "2 min",  120000 },
  { "5 min",  300000 },
  { "10 min", 600000 },
  { "Never",  0      },
};
#define DURATION_COUNT ((uint8_t)(sizeof(DURATIONS) / sizeof(DURATIONS[0])))
#define DURATION_DEFAULT_DIM   2   // 1 min
#define DURATION_DEFAULT_SLEEP 4   // 5 min
