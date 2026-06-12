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
  // FX via Yahoo (<PAIR>=X): near-live market rate + daily prev-close change. Frankfurter (SRC_FRANKFURTER)
  // is still supported but serves only once-daily ECB reference rates, which lag the live market.
  // (KIND_FX_IDR is the frozen enum name for the FX kind; pairs need not involve IDR.)
  {"eur_usd", SRC_YAHOO, "EURUSD=X", "EUR/USD", KIND_FX_IDR, 300, 600, CHG_PREV_CLOSE},
  {"gbp_usd", SRC_YAHOO, "GBPUSD=X", "GBP/USD", KIND_FX_IDR, 300, 600, CHG_PREV_CLOSE},
  {"usd_jpy", SRC_YAHOO, "JPY=X",    "USD/JPY", KIND_FX_IDR, 300, 600, CHG_PREV_CLOSE},
  {"usd_cny", SRC_YAHOO, "CNY=X",    "USD/CNY", KIND_FX_IDR, 300, 600, CHG_PREV_CLOSE},
  {"btc",     SRC_BINANCE,     "BTCUSDT", "BTC",     KIND_CRYPTO, 60,    600,   CHG_24H},
  {"eth",     SRC_BINANCE,     "ETHUSDT", "ETH",     KIND_CRYPTO, 60,    600,   CHG_24H},
  {"sp500",   SRC_YAHOO,       "%5EGSPC", "S&P 500", KIND_INDEX,  300,   600,   CHG_PREV_CLOSE},
  {"nasdaq",  SRC_YAHOO,       "%5EIXIC", "NASDAQ",  KIND_INDEX,  300,   600,   CHG_PREV_CLOSE},
  {"dow",     SRC_YAHOO,       "%5EDJI",  "DOW",     KIND_INDEX,  300,   600,   CHG_PREV_CLOSE},
  {"mag7",    SRC_YAHOO,       "MAGS",    "MAG 7",   KIND_ETF,    300,   600,   CHG_PREV_CLOSE},
  {"gold",    SRC_YAHOO,       "GC=F",    "GOLD",    KIND_INDEX,  300,   600,   CHG_PREV_CLOSE},
  {"oil",     SRC_YAHOO,       "CL=F",    "OIL WTI", KIND_INDEX,  300,   600,   CHG_PREV_CLOSE},
};

#define DEFAULT_TICKERS_COUNT ((uint8_t)(sizeof(DEFAULT_TICKERS) / sizeof(DEFAULT_TICKERS[0])))
