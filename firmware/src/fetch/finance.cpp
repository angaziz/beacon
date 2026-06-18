#include "fetch/finance.h"
#include "fetch/parse_finance.h"
#include "core/net.h"
#include "core/datastore.h"
#include "core/timekeep.h"
#include "core/change_basis.h"
#include "core/fetch_task.h"
#include "config/tickers.h"
#include "util/log.h"
#include <stdio.h>
#include <string.h>

static void publish(uint8_t idx, double value, double change, double change_pct) {
  finance_rec_t f; memset(&f, 0, sizeof(f));
  f.value = value; f.change = change; f.change_pct = change_pct;
  f.hdr.last_updated = (uint32_t)timekeep_now();
  ds_set_finance(idx, &f);   // preserves the seeded id, forces ST_LIVE
}

static data_err_t fail(uint8_t idx, data_err_t e) {
  ds_set_state_finance(idx, e == ERR_NO_ROUTE ? ST_OFFLINE : ST_ERROR, e);
  return e;
}

static data_err_t fetch_binance(uint8_t idx, const ticker_cfg_t* c) {
  char path[96]; int status = 0;
  snprintf(path, sizeof(path), "/api/v3/ticker/24hr?symbol=%s", c->symbol);
  // Binance's public data mirror: same REST shape as api.binance.com, but reachable where the main
  // api host is geo-blocked (e.g. some ID ISPs). AWS-hosted (Starfield Services Root CA - G2).
  data_err_t e = net_https_get("data-api.binance.vision", path, nullptr, nullptr, 0, fetch_scratch(), fetch_scratch_cap(), &status);
  if (e != ERR_NONE) return fail(idx, e);
  double last = 0, pct = 0;
  if (parse_binance(fetch_scratch(), strlen(fetch_scratch()), &last, &pct) != ERR_NONE) return fail(idx, ERR_PARSE);
  double open = (1.0 + pct / 100.0) != 0.0 ? last / (1.0 + pct / 100.0) : last;   // 24h open from pct
  double change = 0; change_compute(last, open, &change, nullptr);
  publish(idx, last, change, pct);   // keep the source's exact 24h pct
  return ERR_NONE;
}

static data_err_t fetch_yahoo(uint8_t idx, const ticker_cfg_t* c) {
  char path[96]; int status = 0;
  snprintf(path, sizeof(path), "/v8/finance/chart/%s?interval=1d&range=1d", c->symbol);
  const char* hk[] = { "User-Agent" };
  const char* hv[] = { "Mozilla/5.0 (compatible; Beacon/1.0)" };   // Yahoo rejects empty UA
  data_err_t e = net_https_get("query1.finance.yahoo.com", path, hk, hv, 1, fetch_scratch(), fetch_scratch_cap(), &status);
  if (e != ERR_NONE) return fail(idx, e);
  double price = 0, prev = 0;
  if (parse_yahoo(fetch_scratch(), strlen(fetch_scratch()), &price, &prev) != ERR_NONE) return fail(idx, ERR_PARSE);
  double change = 0, pct = 0; change_compute(price, prev, &change, &pct);
  publish(idx, price, change, pct);
  return ERR_NONE;
}

data_err_t fetch_finance(uint8_t idx) {
  if (idx >= DEFAULT_TICKERS_COUNT) return ERR_NONE;
  const ticker_cfg_t* c = &DEFAULT_TICKERS[idx];
  switch (c->source) {
    case SRC_BINANCE:     return fetch_binance(idx, c);
    case SRC_YAHOO:       return fetch_yahoo(idx, c);
    default:              return fail(idx, ERR_PARSE);
  }
}
