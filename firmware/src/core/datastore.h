#pragma once
#include <stdint.h>
#include "core/records.h"

// FROZEN API (FR-STATE-0). Thread-safe per tech.md §6: Core-0 fetchers write, Core-1 UI
// reads by-value snapshots; the lock only guards struct copies. Setters force the record
// to ST_LIVE / ERR_NONE on success (clearing any prior error/hub-offline). Callers stamp
// hdr.last_updated (they know the fetch time); the staleness sweep consumes it.

void datastore_init(void);   // seeds finance_count + ids from DEFAULT_TICKERS, all ST_LOADING

// Setters (Core-0). Copy value fields; force hdr.state=ST_LIVE, hdr.err=ERR_NONE.
void ds_set_weather(const weather_rec_t* r);
void ds_set_finance(uint8_t idx, const finance_rec_t* r);  // preserves the slot's seeded id
void ds_set_usage(const usage_rec_t* r);
void ds_set_buddy(const buddy_rec_t* r);

// Explicit failure/transport transitions (do NOT touch the value payload).
void ds_set_state_weather(screen_state_t s, data_err_t e);
void ds_set_state_finance(uint8_t idx, screen_state_t s, data_err_t e);
void ds_set_hub_offline(void);   // flips usage + buddy to ST_HUB_OFFLINE

// Getters (Core-1). By-value snapshot under the lock.
weather_rec_t    ds_get_weather(void);
finance_rec_t    ds_get_finance(uint8_t idx);
uint8_t          ds_get_finance_count(void);
usage_rec_t      ds_get_usage(void);
buddy_rec_t      ds_get_buddy(void);

// Staleness sweep (~1/s from a Core-0 timer). For each record: if state==ST_LIVE and
// record_age_s(hdr, now) >= stale_s(source), promote to ST_STALE. Inclusive boundary.
// Never overwrites ST_OFFLINE / ST_ERROR / ST_HUB_OFFLINE (state-priority rule).
void ds_tick_staleness(uint32_t now);

// Buddy-prompt lifecycle tick (~1/s, Core-0). `now` is MONOTONIC uptime (uptime_s()), not wall clock.
// Mutates prompt only, never hdr.state, so it cannot erase ST_HUB_OFFLINE. Atomic under the lock:
// a SENT_OK beat past BUDDY_CONFIRM_HOLD_S clears (present=false); an undecided (IDLE) prompt past
// BUDDY_PROMPT_EXPIRY_S becomes PROMPT_TOO_LATE (reuses the dismiss affordance, no new state).
void ds_tick_buddy_prompt(uint32_t now);
