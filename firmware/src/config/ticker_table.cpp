#include "config/ticker_table.h"
#include "core/ds_lock.h"   // same std::mutex (native) / FreeRTOS (device) wrapper as DataStore
#include <string.h>

// Dedicated lock for the runtime table — separate from DataStore's s_lock (design §3.1).
static ds_lock_t        s_lock;
static ticker_runtime_t s_tickers[MAX_TICKERS];
static int              s_count;
static uint32_t         s_gen;
static bool             s_inited;

static void seed_from_defaults(void) {
  int n = DEFAULT_TICKERS_COUNT;
  if (n > MAX_TICKERS) n = MAX_TICKERS;
  memset(s_tickers, 0, sizeof(s_tickers));
  for (int i = 0; i < n; i++) {
    const ticker_cfg_t* c = &DEFAULT_TICKERS[i];
    ticker_runtime_t* t = &s_tickers[i];
    strncpy(t->id, c->id, FIN_ID_LEN - 1);
    strncpy(t->symbol, c->symbol, TKR_SYM_LEN - 1);
    strncpy(t->name, c->display_name, TKR_NAME_LEN - 1);
    t->source = c->source;
    t->kind = c->kind;
    t->cadence_s = c->cadence_s;
    t->stale_s = c->stale_s;
    t->change_basis = c->change_basis;
  }
  s_count = n;
}

void ticker_table_init(void) {
  if (!s_inited) { ds_lock_init(s_lock); s_inited = true; }
  ds_lock_take(s_lock);
  seed_from_defaults();
  s_gen = 0;   // no swap yet (A5 bumps on hot-swap)
  ds_lock_give(s_lock);
}

int ticker_table_count(void) {
  ds_lock_take(s_lock); int c = s_count; ds_lock_give(s_lock); return c;
}

bool ticker_table_get(int i, ticker_runtime_t* out) {
  if (!out) return false;
  ds_lock_take(s_lock);
  bool ok = (i >= 0 && i < s_count);
  if (ok) *out = s_tickers[i];   // copy under lock; caller owns the snapshot
  ds_lock_give(s_lock);
  return ok;
}

uint32_t ticker_table_gen(void) {
  ds_lock_take(s_lock); uint32_t g = s_gen; ds_lock_give(s_lock); return g;
}
