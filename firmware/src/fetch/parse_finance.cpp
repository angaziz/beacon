#include "fetch/parse_finance.h"
#include <ArduinoJson.h>
#include <stdlib.h>   // atof

data_err_t parse_binance(const char* json, size_t len, double* last, double* change_pct) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return ERR_PARSE;
  const char* lp = doc["lastPrice"];                 // Binance encodes numbers as strings
  if (!lp) return ERR_PARSE;
  const char* pc = doc["priceChangePercent"];
  if (last)       *last = atof(lp);
  if (change_pct) *change_pct = pc ? atof(pc) : 0.0;
  return ERR_NONE;
}

data_err_t parse_yahoo(const char* json, size_t len, double* price, double* prev_close) {
  // Filter: materialize only chart.result[].meta (payload also carries timestamp/indicator arrays).
  JsonDocument filter;
  filter["chart"]["result"][0]["meta"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, json, len, DeserializationOption::Filter(filter))) return ERR_PARSE;
  JsonObjectConst meta = doc["chart"]["result"][0]["meta"];
  if (meta.isNull() || meta["regularMarketPrice"].isNull()) return ERR_PARSE;
  double p = meta["regularMarketPrice"].as<double>();
  if (price) *price = p;
  if (prev_close) {
    JsonVariantConst pc = meta["previousClose"];
    if (pc.isNull()) pc = meta["chartPreviousClose"];   // Yahoo varies by symbol/session
    *prev_close = pc.isNull() ? p : pc.as<double>();     // no basis => 0 change (== price)
  }
  return ERR_NONE;
}
