#include <unity.h>
#include <ArduinoJson.h>
#include <string.h>
#include "core/hub_proto.h"

void setUp(void) {}
void tearDown(void) {}

static ticker_runtime_t mkrow(const char* id, ticker_source_t src, const char* sym,
                              const char* name, ticker_kind_t kind, uint16_t cad,
                              uint32_t stale, change_basis_t basis) {
  ticker_runtime_t r; memset(&r, 0, sizeof(r));
  strncpy(r.id, id, FIN_ID_LEN - 1);
  r.source = src;
  strncpy(r.symbol, sym, TKR_SYM_LEN - 1);
  strncpy(r.name, name, TKR_NAME_LEN - 1);
  r.kind = kind; r.cadence_s = cad; r.stale_s = stale; r.change_basis = basis;
  return r;
}

// ===== single-chunk frame shape =====
static void test_single_chunk_shape(void) {
  ticker_runtime_t rows[2] = {
    mkrow("bz_btcusdt", SRC_BINANCE, "BTCUSDT", "BTC", KIND_CRYPTO, 60, 600, CHG_24H),
    mkrow("yh_gspc", SRC_YAHOO, "%5EGSPC", "S&P 500", KIND_INDEX, 300, 600, CHG_PREV_CLOSE),
  };
  int gs[MAX_TICKERS];
  TEST_ASSERT_EQUAL_INT(1, hub_report_plan(rows, 2, gs));   // both rows fit one chunk
  TEST_ASSERT_EQUAL_INT(0, gs[0]);

  char buf[HUB_FRAME_MAX];
  size_t n = hub_build_report_frame(rows, 0, 2, 0, 1, buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_CHAR('\n', buf[n - 1]);                 // newline-terminated

  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, buf));   // DeserializationError has operator bool, no int cast
  TEST_ASSERT_EQUAL_INT(1, doc["v"].as<int>());
  TEST_ASSERT_EQUAL_STRING("report", doc["cmd"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("tickers", doc["what"].as<const char*>());
  TEST_ASSERT_EQUAL_INT(0, doc["rev"].as<int>());           // rev always 0
  TEST_ASSERT_EQUAL_INT(0, doc["part"].as<int>());
  TEST_ASSERT_EQUAL_INT(1, doc["parts"].as<int>());

  JsonArrayConst t = doc["tickers"].as<JsonArrayConst>();
  TEST_ASSERT_EQUAL_INT(2, (int)t.size());
  TEST_ASSERT_EQUAL_STRING("bz_btcusdt", t[0]["id"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("binance",    t[0]["src"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("BTCUSDT",    t[0]["sym"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("BTC",        t[0]["name"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("crypto",     t[0]["kind"].as<const char*>());
  TEST_ASSERT_EQUAL_INT(60,  t[0]["cadence"].as<int>());
  TEST_ASSERT_EQUAL_INT(600, t[0]["stale"].as<int>());
  TEST_ASSERT_EQUAL_STRING("24h",        t[0]["basis"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("yahoo",      t[1]["src"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("%5EGSPC",    t[1]["sym"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("index",      t[1]["kind"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("prev_close", t[1]["basis"].as<const char*>());
}

// ===== enum -> wire mapping matrix =====
static void test_enum_to_wire(void) {
  struct { ticker_kind_t k; const char* s; } kinds[] = {
    {KIND_FX, "fx"}, {KIND_CRYPTO, "crypto"}, {KIND_INDEX, "index"}, {KIND_ETF, "etf"},
  };
  for (int i = 0; i < 4; i++) {
    ticker_runtime_t r = mkrow("id", SRC_YAHOO, "X", "X", kinds[i].k, 300, 600, CHG_PREV_CLOSE);
    char buf[HUB_FRAME_MAX];
    size_t n = hub_build_report_frame(&r, 0, 1, 0, 1, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    JsonDocument doc; deserializeJson(doc, buf);
    TEST_ASSERT_EQUAL_STRING(kinds[i].s, doc["tickers"][0]["kind"].as<const char*>());
  }
}

// ===== multi-chunk split (whole rows only) =====
static void test_multi_chunk_split(void) {
  // 16 rows with long names/symbols => must span >1 chunk under the 900B budget.
  ticker_runtime_t rows[MAX_TICKERS];
  for (int i = 0; i < MAX_TICKERS; i++)
    rows[i] = mkrow("idididididid", SRC_YAHOO, "SYMSYMSYMSYMSYMSYMSYM=X",
                    "LONGNAME LONGNAME NAME", KIND_INDEX, 300, 600, CHG_PREV_CLOSE);
  int gs[MAX_TICKERS];
  int parts = hub_report_plan(rows, MAX_TICKERS, gs);
  TEST_ASSERT_TRUE(parts >= 2);                              // genuinely chunked
  TEST_ASSERT_EQUAL_INT(0, gs[0]);
  for (int g = 1; g < parts; g++) TEST_ASSERT_TRUE(gs[g] > gs[g - 1]);   // strictly increasing

  // Every chunk serializes within budget and carries the right part/parts.
  for (int g = 0; g < parts; g++) {
    int lo = gs[g];
    int hi = (g + 1 < parts) ? gs[g + 1] : MAX_TICKERS;
    char buf[HUB_FRAME_MAX];
    size_t n = hub_build_report_frame(rows, lo, hi, g, parts, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0 && n <= 900);
    JsonDocument doc; deserializeJson(doc, buf);
    TEST_ASSERT_EQUAL_INT(g, doc["part"].as<int>());
    TEST_ASSERT_EQUAL_INT(parts, doc["parts"].as<int>());
  }
}

// ===== count guards =====
static void test_count_guards(void) {
  ticker_runtime_t r = mkrow("a", SRC_YAHOO, "A", "A", KIND_FX, 300, 600, CHG_PREV_CLOSE);
  int gs[MAX_TICKERS];
  TEST_ASSERT_EQUAL_INT(0, hub_report_plan(&r, 0, gs));                 // count < 1
  TEST_ASSERT_EQUAL_INT(0, hub_report_plan(&r, MAX_TICKERS + 1, gs));   // count > MAX_TICKERS
  TEST_ASSERT_EQUAL_INT(0, hub_report_plan(NULL, 1, gs));               // null rows
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_single_chunk_shape);
  RUN_TEST(test_enum_to_wire);
  RUN_TEST(test_multi_chunk_split);
  RUN_TEST(test_count_guards);
  return UNITY_END();
}
