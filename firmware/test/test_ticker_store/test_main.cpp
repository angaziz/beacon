#include <unity.h>
#include <string.h>
#include "config/ticker_store.h"
#include "config/ticker_table.h"
#include "config/tickers.h"

void setUp(void) {}
void tearDown(void) {}

static ticker_runtime_t mk(const char* id, ticker_source_t src, const char* sym, const char* name,
                           ticker_kind_t kind, uint16_t cad, uint32_t stale, change_basis_t b) {
  ticker_runtime_t t; memset(&t, 0, sizeof(t));
  strncpy(t.id, id, FIN_ID_LEN - 1);
  strncpy(t.symbol, sym, TKR_SYM_LEN - 1);
  strncpy(t.name, name, TKR_NAME_LEN - 1);
  t.source = src; t.kind = kind; t.cadence_s = cad; t.stale_s = stale; t.change_basis = b;
  return t;
}

// Representative multi-row set: both sources, all four kinds, both bases.
static int sample_rows(ticker_runtime_t* r) {
  r[0] = mk("btc",     SRC_BINANCE, "BTCUSDT", "BTC",     KIND_CRYPTO, 60,  600, CHG_24H);
  r[1] = mk("eur_usd", SRC_YAHOO,   "EURUSD=X","EUR/USD", KIND_FX,     300, 600, CHG_PREV_CLOSE);
  r[2] = mk("sp500",   SRC_YAHOO,   "%5EGSPC", "S&P 500", KIND_INDEX,  300, 600, CHG_PREV_CLOSE);
  r[3] = mk("mag7",    SRC_YAHOO,   "MAGS",    "MAG 7",   KIND_ETF,    300, 900, CHG_PREV_CLOSE);
  return 4;
}

static void assert_row_eq(const ticker_runtime_t* a, const ticker_runtime_t* b) {
  TEST_ASSERT_EQUAL_STRING(a->id, b->id);
  TEST_ASSERT_EQUAL_STRING(a->symbol, b->symbol);
  TEST_ASSERT_EQUAL_STRING(a->name, b->name);
  TEST_ASSERT_EQUAL_INT(a->source, b->source);
  TEST_ASSERT_EQUAL_INT(a->kind, b->kind);
  TEST_ASSERT_EQUAL_INT(a->change_basis, b->change_basis);
  TEST_ASSERT_EQUAL_UINT16(a->cadence_s, b->cadence_s);
  TEST_ASSERT_EQUAL_UINT32(a->stale_s, b->stale_s);
}

static void test_pack_unpack_roundtrip(void) {
  ticker_runtime_t in[MAX_TICKERS]; int n = sample_rows(in);
  uint8_t buf[TICKER_BLOB_MAX];
  size_t w = ticker_store_pack(in, n, buf, sizeof(buf));
  TEST_ASSERT_TRUE(w > 0);

  ticker_runtime_t out[MAX_TICKERS];
  int got = ticker_store_unpack(buf, w, out, MAX_TICKERS);
  TEST_ASSERT_EQUAL_INT(n, got);
  for (int i = 0; i < n; i++) assert_row_eq(&in[i], &out[i]);
}

static void test_reject_bad_crc(void) {
  ticker_runtime_t in[MAX_TICKERS]; int n = sample_rows(in);
  uint8_t buf[TICKER_BLOB_MAX];
  size_t w = ticker_store_pack(in, n, buf, sizeof(buf));
  buf[w - 1] ^= 0xFF;   // corrupt a body byte => crc mismatch
  ticker_runtime_t out[MAX_TICKERS];
  TEST_ASSERT_EQUAL_INT(-1, ticker_store_unpack(buf, w, out, MAX_TICKERS));
}

static void test_reject_bad_schema_ver(void) {
  ticker_runtime_t in[MAX_TICKERS]; int n = sample_rows(in);
  uint8_t buf[TICKER_BLOB_MAX];
  size_t w = ticker_store_pack(in, n, buf, sizeof(buf));
  buf[0] = TICKER_STORE_SCHEMA_VER + 1;   // future/unknown version
  ticker_runtime_t out[MAX_TICKERS];
  TEST_ASSERT_EQUAL_INT(-1, ticker_store_unpack(buf, w, out, MAX_TICKERS));
}

static void test_reject_truncated(void) {
  ticker_runtime_t in[MAX_TICKERS]; int n = sample_rows(in);
  uint8_t buf[TICKER_BLOB_MAX];
  size_t w = ticker_store_pack(in, n, buf, sizeof(buf));
  ticker_runtime_t out[MAX_TICKERS];
  TEST_ASSERT_EQUAL_INT(-1, ticker_store_unpack(buf, w - 1, out, MAX_TICKERS));   // one byte short
  TEST_ASSERT_EQUAL_INT(-1, ticker_store_unpack(buf, 3, out, MAX_TICKERS));        // below header
}

static void test_reject_count_over_max(void) {
  ticker_runtime_t in[MAX_TICKERS]; int n = sample_rows(in);
  uint8_t buf[TICKER_BLOB_MAX];
  size_t w = ticker_store_pack(in, n, buf, sizeof(buf));
  buf[1] = MAX_TICKERS + 1;   // claim too many rows (len no longer matches => reject)
  ticker_runtime_t out[MAX_TICKERS];
  TEST_ASSERT_EQUAL_INT(-1, ticker_store_unpack(buf, w, out, MAX_TICKERS));
}

// Each table row flips one code byte to an unknown value and re-stamps the crc, proving each
// of src/kind/basis is independently rejected (a clean-crc blob with an unknown code => -1).
static void test_reject_unknown_codes(void) {
  struct { const char* label; int code_offset; } cases[] = {
    {"src",   FIN_ID_LEN + TKR_SYM_LEN + TKR_NAME_LEN + 0},
    {"kind",  FIN_ID_LEN + TKR_SYM_LEN + TKR_NAME_LEN + 1},
    {"basis", FIN_ID_LEN + TKR_SYM_LEN + TKR_NAME_LEN + 2},
  };
  for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
    ticker_runtime_t in[MAX_TICKERS]; int n = sample_rows(in);
    uint8_t buf[TICKER_BLOB_MAX];
    size_t w = ticker_store_pack(in, n, buf, sizeof(buf));
    uint8_t* body = buf + 6;
    body[cases[k].code_offset] = 0x7F;   // unknown code
    extern uint32_t ticker_store_crc32(const uint8_t*, size_t);
    uint32_t crc = ticker_store_crc32(body, (size_t)n * TICKER_ENTRY_BYTES);
    buf[2] = crc & 0xFF; buf[3] = (crc >> 8) & 0xFF; buf[4] = (crc >> 16) & 0xFF; buf[5] = (crc >> 24) & 0xFF;
    ticker_runtime_t out[MAX_TICKERS];
    int got = ticker_store_unpack(buf, w, out, MAX_TICKERS);
    if (got != -1) TEST_FAIL_MESSAGE(cases[k].label);   // unknown code must be rejected
  }
}

// STABLE-CODE PROOF: wire code 1 must decode to SRC_BINANCE and 2 to SRC_YAHOO regardless of the
// underlying C enum ordinals. A future enum reorder cannot silently remap these.
static void test_stable_codes_decoupled_from_ordinals(void) {
  ticker_runtime_t in[MAX_TICKERS];
  in[0] = mk("a", SRC_BINANCE, "X", "X", KIND_FX, 60, 600, CHG_PREV_CLOSE);
  in[1] = mk("b", SRC_YAHOO,   "Y", "Y", KIND_FX, 60, 600, CHG_PREV_CLOSE);
  uint8_t buf[TICKER_BLOB_MAX];
  size_t w = ticker_store_pack(in, 2, buf, sizeof(buf));

  // The src code byte sits right after the three fixed string fields in each entry.
  size_t code0 = 6 + (FIN_ID_LEN + TKR_SYM_LEN + TKR_NAME_LEN);
  size_t code1 = code0 + TICKER_ENTRY_BYTES;
  TEST_ASSERT_EQUAL_UINT8(1, buf[code0]);   // binance => 1 on the wire, not its ordinal
  TEST_ASSERT_EQUAL_UINT8(2, buf[code1]);   // yahoo   => 2 on the wire

  ticker_runtime_t out[MAX_TICKERS];
  TEST_ASSERT_EQUAL_INT(2, ticker_store_unpack(buf, w, out, MAX_TICKERS));
  TEST_ASSERT_EQUAL_INT(SRC_BINANCE, out[0].source);
  TEST_ASSERT_EQUAL_INT(SRC_YAHOO,   out[1].source);
}

static void test_pack_rejects_bad_count_and_overflow(void) {
  ticker_runtime_t in[MAX_TICKERS]; sample_rows(in);
  uint8_t buf[TICKER_BLOB_MAX];
  TEST_ASSERT_EQUAL_size_t(0, ticker_store_pack(in, 0, buf, sizeof(buf)));              // count < 1
  TEST_ASSERT_EQUAL_size_t(0, ticker_store_pack(in, MAX_TICKERS + 1, buf, sizeof(buf))); // count > MAX
  TEST_ASSERT_EQUAL_size_t(0, ticker_store_pack(in, 4, buf, 8));                        // cap overflow
}

// NOTE: ticker_table_init() round-trip after a save is NOT exercised here — the NVS wrappers
// (ticker_store_save/load) are device-only (Preferences absent on the native host, stubbed to
// false/-1). On native, init always falls back to DEFAULT_TICKERS. The device I/O path is
// verified on-device per the A5 manual checklist.

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pack_unpack_roundtrip);
  RUN_TEST(test_reject_bad_crc);
  RUN_TEST(test_reject_bad_schema_ver);
  RUN_TEST(test_reject_truncated);
  RUN_TEST(test_reject_count_over_max);
  RUN_TEST(test_reject_unknown_codes);
  RUN_TEST(test_stable_codes_decoupled_from_ordinals);
  RUN_TEST(test_pack_rejects_bad_count_and_overflow);
  return UNITY_END();
}
