#pragma once
#include <stdint.h>

// FROZEN schema (FR-STATE-0). Values stay editable; NVS override arrives in chunk D.
typedef enum { SRC_FRANKFURTER, SRC_BINANCE, SRC_YAHOO } ticker_source_t;
typedef enum { KIND_FX_IDR, KIND_CRYPTO, KIND_INDEX, KIND_ETF } ticker_kind_t;
typedef enum { CHG_PREV_CLOSE, CHG_24H } change_basis_t;

typedef struct {
  const char*     id;           // stable key (NVS/DataStore/config linkage); never reuse
  ticker_source_t source;
  const char*     symbol;       // source-specific symbol (e.g. "%5EGSPC" for Yahoo S&P 500)
  const char*     display_name; // shown on the Finance screen
  ticker_kind_t   kind;
  uint16_t        cadence_s;    // fetch period
  uint32_t        stale_s;      // age (s) after which the slot is ST_STALE
  change_basis_t  change_basis;
} ticker_cfg_t;

#define MAX_TICKERS 16

// === EDIT HERE to add/remove/reorder instruments. id must stay unique + stable. ===
// Fields: id, source, symbol, display_name, kind, cadence_s, stale_s, change_basis
static const ticker_cfg_t DEFAULT_TICKERS[] = {
  {"usd_idr", SRC_FRANKFURTER, "USD",     "USD/IDR", KIND_FX_IDR, 21600, 86400, CHG_PREV_CLOSE},
  {"eur_idr", SRC_FRANKFURTER, "EUR",     "EUR/IDR", KIND_FX_IDR, 21600, 86400, CHG_PREV_CLOSE},
  {"sgd_idr", SRC_FRANKFURTER, "SGD",     "SGD/IDR", KIND_FX_IDR, 21600, 86400, CHG_PREV_CLOSE},
  {"jpy_idr", SRC_FRANKFURTER, "JPY",     "JPY/IDR", KIND_FX_IDR, 21600, 86400, CHG_PREV_CLOSE},
  {"cny_idr", SRC_FRANKFURTER, "CNY",     "CNY/IDR", KIND_FX_IDR, 21600, 86400, CHG_PREV_CLOSE},
  {"btc",     SRC_BINANCE,     "BTCUSDT", "BTC",     KIND_CRYPTO, 60,    600,   CHG_24H},
  {"sp500",   SRC_YAHOO,       "%5EGSPC", "S&P 500", KIND_INDEX,  300,   600,   CHG_PREV_CLOSE},
  {"nasdaq",  SRC_YAHOO,       "%5EIXIC", "NASDAQ",  KIND_INDEX,  300,   600,   CHG_PREV_CLOSE},
  {"arkk",    SRC_YAHOO,       "ARKK",    "ARKK",    KIND_ETF,    300,   600,   CHG_PREV_CLOSE},
  {"ihsg",    SRC_YAHOO,       "%5EJKSE", "IHSG",    KIND_INDEX,  300,   600,   CHG_PREV_CLOSE},
};

#define DEFAULT_TICKERS_COUNT ((uint8_t)(sizeof(DEFAULT_TICKERS) / sizeof(DEFAULT_TICKERS[0])))
