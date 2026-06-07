#pragma once
#include <stdint.h>
#include <stdio.h>
#include "core/screen_state.h"
#include "core/records.h"

// Compact age string: "now"/"12s"/"5m"/"3h"/"2d". UINT32_MAX => "--".
static inline void age_str(char* buf, size_t n, uint32_t age_s) {
  if (age_s == UINT32_MAX)      snprintf(buf, n, "--");
  else if (age_s < 5)           snprintf(buf, n, "now");
  else if (age_s < 60)          snprintf(buf, n, "%us", (unsigned)age_s);
  else if (age_s < 3600)        snprintf(buf, n, "%um", (unsigned)(age_s / 60));
  else if (age_s < 86400)       snprintf(buf, n, "%uh", (unsigned)(age_s / 3600));
  else                          snprintf(buf, n, "%ud", (unsigned)(age_s / 86400));
}

// Countdown to a reset epoch (usage windows): "2h08"/"45m"/"30s"/"--". reset==0 or past => "--".
// In dev the seed reset shares the millis/1000 base with `now`; P0-D swaps in real epoch time.
static inline void reset_str(char* buf, size_t n, uint32_t reset, uint32_t now) {
  if (reset == 0 || reset <= now) { snprintf(buf, n, "--"); return; }
  uint32_t s = reset - now;
  if (s < 60)          snprintf(buf, n, "%us", (unsigned)s);
  else if (s < 3600)   snprintf(buf, n, "%um", (unsigned)(s / 60));
  else if (s < 86400)  snprintf(buf, n, "%uh%02u", (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
  else                 snprintf(buf, n, "%ud", (unsigned)(s / 86400));
}

// Status-slot text for a record's state. Returns true if a non-live chip should show
// (caller swaps the live right-header for `buf`); false => LIVE, show the normal header.
static inline bool sv_status(char* buf, size_t n, const record_hdr_t* h, uint32_t now) {
  char age[8];
  switch (h->state) {
    case ST_LIVE:        return false;
    case ST_LOADING:     snprintf(buf, n, "..."); return true;
    case ST_STALE:       age_str(age, sizeof(age), record_age_s(h, now)); snprintf(buf, n, "STALE %s", age); return true;
    case ST_OFFLINE:     snprintf(buf, n, "OFFLINE"); return true;
    case ST_HUB_OFFLINE: snprintf(buf, n, "HUB OFFLINE"); return true;
    case ST_RECONNECTING:snprintf(buf, n, "RECONNECTING"); return true;
    case ST_ERROR:
      switch (h->err) {
        case ERR_RATE_LIMITED: snprintf(buf, n, "RATE LIMIT"); break;
        case ERR_TIMEOUT:      snprintf(buf, n, "TIMEOUT"); break;
        case ERR_NO_ROUTE:     snprintf(buf, n, "NO ROUTE"); break;
        default:               snprintf(buf, n, "ERR"); break;
      }
      return true;
    default:             snprintf(buf, n, "ERR"); return true;
  }
}

// Value should render as dim (stale/offline/error keep last value, dimmed).
static inline bool sv_dim(screen_state_t s) {
  return s == ST_STALE || s == ST_OFFLINE || s == ST_ERROR || s == ST_HUB_OFFLINE || s == ST_RECONNECTING;
}
// Value should render as placeholder dashes (no usable value yet).
static inline bool sv_placeholder(screen_state_t s) { return s == ST_LOADING; }
// Severe states color the chip with the down/alert color (vs ink_dim for stale age).
static inline bool sv_severe(screen_state_t s) {
  return s == ST_OFFLINE || s == ST_ERROR || s == ST_HUB_OFFLINE;
}
