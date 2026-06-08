#pragma once
#include <stddef.h>
#include "core/screen_state.h"   // data_err_t

// Pure, host-testable parsers for the three finance sources (tech.md §6 / docs/research §2.3).
// Each fills numeric outputs only; the caller maps to a finance_rec_t + change_basis + state.
#ifdef __cplusplus
extern "C" {
#endif

// Binance /api/v3/ticker/24hr: {"lastPrice":"62392.1","priceChangePercent":"2.14", ...} (string numbers).
data_err_t parse_binance(const char* json, size_t len, double* last, double* change_pct);

// Frankfurter /v1/latest?base=<X>&symbols=IDR: {"rates":{"IDR":16320.5}, ...} => X/IDR rate.
data_err_t parse_frankfurter(const char* json, size_t len, double* rate);

// Frankfurter timeseries /v1/<start>..<end>?base=<X>&symbols=IDR:
// {"rates":{"2026-06-04":{"IDR":18026},"2026-06-05":{"IDR":18070}}} => latest + previous business day
// (the dated single-call approach collides over weekends; the series yields a real prev-close change).
data_err_t parse_frankfurter_series(const char* json, size_t len, double* latest, double* prev);

// Yahoo /v8/finance/chart/<sym>: chart.result[0].meta.{regularMarketPrice, previousClose}.
// Uses an ArduinoJson filter so only meta is materialized (the raw payload is multi-KB).
data_err_t parse_yahoo(const char* json, size_t len, double* price, double* prev_close);

#ifdef __cplusplus
}
#endif
