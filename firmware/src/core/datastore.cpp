#include "core/datastore.h"
#include "core/ds_lock.h"
#include "core/stale.h"
#include "config/tickers.h"
#include <string.h>

static ds_lock_t        s_lock;
static weather_rec_t    s_weather;
static finance_rec_t    s_finance[MAX_TICKERS];
static uint8_t          s_finance_count;
static usage_rec_t      s_usage;
static buddy_rec_t      s_buddy;
static nowplaying_rec_t s_nowplaying;

static void hdr_loading(record_hdr_t* h) { h->last_updated = 0; h->state = ST_LOADING; h->err = ERR_NONE; }

void datastore_init(void) {
  ds_lock_init(s_lock);
  ds_lock_take(s_lock);
  memset(&s_weather, 0, sizeof(s_weather));       hdr_loading(&s_weather.hdr);
  memset(&s_usage, 0, sizeof(s_usage));           hdr_loading(&s_usage.hdr);
  memset(&s_buddy, 0, sizeof(s_buddy));           hdr_loading(&s_buddy.hdr);
  memset(&s_nowplaying, 0, sizeof(s_nowplaying));  hdr_loading(&s_nowplaying.hdr);

  s_finance_count = DEFAULT_TICKERS_COUNT;
  if (s_finance_count > MAX_TICKERS) s_finance_count = MAX_TICKERS;
  memset(s_finance, 0, sizeof(s_finance));
  for (uint8_t i = 0; i < s_finance_count; i++) {
    strncpy(s_finance[i].id, DEFAULT_TICKERS[i].id, FIN_ID_LEN - 1);
    s_finance[i].id[FIN_ID_LEN - 1] = 0;
    hdr_loading(&s_finance[i].hdr);
  }
  ds_lock_give(s_lock);
}

// --- setters: copy value, force LIVE/NONE (success clears prior error/hub-offline) ---
void ds_set_weather(const weather_rec_t* r) {
  ds_lock_take(s_lock);
  s_weather = *r; s_weather.hdr.state = ST_LIVE; s_weather.hdr.err = ERR_NONE;
  ds_lock_give(s_lock);
}
void ds_set_finance(uint8_t idx, const finance_rec_t* r) {
  if (idx >= MAX_TICKERS) return;
  ds_lock_take(s_lock);
  char id[FIN_ID_LEN]; strncpy(id, s_finance[idx].id, FIN_ID_LEN); id[FIN_ID_LEN - 1] = 0;
  s_finance[idx] = *r;
  strncpy(s_finance[idx].id, id, FIN_ID_LEN); s_finance[idx].id[FIN_ID_LEN - 1] = 0; // keep seeded id
  s_finance[idx].hdr.state = ST_LIVE; s_finance[idx].hdr.err = ERR_NONE;
  ds_lock_give(s_lock);
}
void ds_set_usage(const usage_rec_t* r) {
  ds_lock_take(s_lock);
  s_usage = *r; s_usage.hdr.state = ST_LIVE; s_usage.hdr.err = ERR_NONE;
  ds_lock_give(s_lock);
}
void ds_set_buddy(const buddy_rec_t* r) {
  ds_lock_take(s_lock);
  s_buddy = *r; s_buddy.hdr.state = ST_LIVE; s_buddy.hdr.err = ERR_NONE;
  ds_lock_give(s_lock);
}
void ds_set_nowplaying(const nowplaying_rec_t* r) {
  ds_lock_take(s_lock);
  s_nowplaying = *r; s_nowplaying.hdr.state = ST_LIVE; s_nowplaying.hdr.err = ERR_NONE;
  ds_lock_give(s_lock);
}

// --- explicit state transitions: do not touch value payload ---
void ds_set_state_weather(screen_state_t s, data_err_t e) {
  ds_lock_take(s_lock); s_weather.hdr.state = s; s_weather.hdr.err = e; ds_lock_give(s_lock);
}
void ds_set_state_finance(uint8_t idx, screen_state_t s, data_err_t e) {
  if (idx >= MAX_TICKERS) return;
  ds_lock_take(s_lock); s_finance[idx].hdr.state = s; s_finance[idx].hdr.err = e; ds_lock_give(s_lock);
}
void ds_set_state_nowplaying(screen_state_t s, data_err_t e) {
  ds_lock_take(s_lock); s_nowplaying.hdr.state = s; s_nowplaying.hdr.err = e; ds_lock_give(s_lock);
}
void ds_set_hub_offline(void) {
  ds_lock_take(s_lock);
  s_usage.hdr.state = ST_HUB_OFFLINE;
  s_buddy.hdr.state = ST_HUB_OFFLINE;
  ds_lock_give(s_lock);
}

// --- getters: by-value snapshots ---
weather_rec_t ds_get_weather(void) {
  ds_lock_take(s_lock); weather_rec_t r = s_weather; ds_lock_give(s_lock); return r;
}
finance_rec_t ds_get_finance(uint8_t idx) {
  finance_rec_t r; memset(&r, 0, sizeof(r));
  if (idx >= MAX_TICKERS) { hdr_loading(&r.hdr); return r; }
  ds_lock_take(s_lock); r = s_finance[idx]; ds_lock_give(s_lock); return r;
}
uint8_t ds_get_finance_count(void) {
  ds_lock_take(s_lock); uint8_t c = s_finance_count; ds_lock_give(s_lock); return c;
}
usage_rec_t ds_get_usage(void) {
  ds_lock_take(s_lock); usage_rec_t r = s_usage; ds_lock_give(s_lock); return r;
}
buddy_rec_t ds_get_buddy(void) {
  ds_lock_take(s_lock); buddy_rec_t r = s_buddy; ds_lock_give(s_lock); return r;
}
nowplaying_rec_t ds_get_nowplaying(void) {
  ds_lock_take(s_lock); nowplaying_rec_t r = s_nowplaying; ds_lock_give(s_lock); return r;
}

// --- staleness sweep ---
static void sweep_one(record_hdr_t* h, uint32_t now, uint32_t stale_s) {
  if (h->state == ST_LIVE && record_age_s(h, now) >= stale_s) h->state = ST_STALE;
}
void ds_tick_staleness(uint32_t now) {
  ds_lock_take(s_lock);
  sweep_one(&s_weather.hdr,    now, WEATHER_STALE_S);
  sweep_one(&s_usage.hdr,      now, USAGE_STALE_S);
  sweep_one(&s_buddy.hdr,      now, BUDDY_STALE_S);
  sweep_one(&s_nowplaying.hdr, now, NOWPLAYING_STALE_S);
  for (uint8_t i = 0; i < s_finance_count; i++) sweep_one(&s_finance[i].hdr, now, finance_stale_s(i));
  ds_lock_give(s_lock);
}

// `now` is monotonic uptime (uptime_s()); shown_at/decided_at share that epoch (records.h).
void ds_tick_buddy_prompt(uint32_t now) {
  ds_lock_take(s_lock);
  buddy_prompt_t* p = &s_buddy.prompt;
  if (p->present) {
    if (p->decision_state == PROMPT_SENT_OK) {
      if (now - p->decided_at >= BUDDY_CONFIRM_HOLD_S) p->present = false;        // beat shown; clear
    } else if (p->decision_state == PROMPT_IDLE_DECISION) {
      if (now - p->shown_at >= BUDDY_PROMPT_EXPIRY_S) p->decision_state = PROMPT_TOO_LATE;   // expired
    }
  }
  ds_lock_give(s_lock);
}
