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

// Yahoo /v8/finance/chart/<sym>: chart.result[0].meta.{regularMarketPrice, previousClose}.
// Uses an ArduinoJson filter so only meta is materialized (the raw payload is multi-KB).
data_err_t parse_yahoo(const char* json, size_t len, double* price, double* prev_close);

#ifdef __cplusplus
}
#endif
