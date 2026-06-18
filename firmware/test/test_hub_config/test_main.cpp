#include <unity.h>
#include <string.h>
#include <stdio.h>
#include "core/hub_proto.h"

void setUp(void) {}
void tearDown(void) {}

// Feed one frame through parse + accumulate; returns the accumulator status and the err string.
static config_status_t feed(config_accum_t* acc, const char* json, const char** err) {
  config_chunk_t ch;
  memset(&ch, 0, sizeof(ch));
  *err = NULL;
  data_err_t pe = hub_parse_config_chunk(json, strlen(json), &ch, err);
  if (pe != ERR_NONE) return CFG_ERR;            // a parse/row error surfaces the same err channel
  return hub_config_accum_step(acc, &ch, err);
}

// ===== single-part valid snapshot =====

static void test_single_part_valid(void) {
  const char* j =
    "{\"v\":1,\"config\":{\"rev\":7,\"part\":0,\"parts\":1,\"tickers\":["
    "{\"id\":\"bz_btcusdt\",\"src\":\"binance\",\"sym\":\"BTCUSDT\",\"name\":\"BTC\","
    "\"kind\":\"crypto\",\"cadence\":60,\"stale\":600,\"basis\":\"24h\"},"
    "{\"id\":\"yh_gspc\",\"src\":\"yahoo\",\"sym\":\"%5EGSPC\",\"name\":\"S&P 500\","
    "\"kind\":\"index\",\"cadence\":300,\"stale\":600,\"basis\":\"prev_close\"}]}}";
  config_accum_t acc; memset(&acc, 0, sizeof(acc));
  const char* err;
  TEST_ASSERT_EQUAL_INT(CFG_DONE, feed(&acc, j, &err));
  TEST_ASSERT_EQUAL_INT(2, acc.row_count);
  TEST_ASSERT_EQUAL_UINT32(7, acc.rev);

  // binance crypto/24h row
  TEST_ASSERT_EQUAL_STRING("bz_btcusdt", acc.rows[0].id);
  TEST_ASSERT_EQUAL_STRING("BTCUSDT", acc.rows[0].symbol);
  TEST_ASSERT_EQUAL_STRING("BTC", acc.rows[0].name);
  TEST_ASSERT_EQUAL_INT(SRC_BINANCE, acc.rows[0].source);
  TEST_ASSERT_EQUAL_INT(KIND_CRYPTO, acc.rows[0].kind);
  TEST_ASSERT_EQUAL_INT(CHG_24H, acc.rows[0].change_basis);
  TEST_ASSERT_EQUAL_UINT16(60, acc.rows[0].cadence_s);
  TEST_ASSERT_EQUAL_UINT32(600, acc.rows[0].stale_s);

  // yahoo index/prev_close row
  TEST_ASSERT_EQUAL_STRING("yh_gspc", acc.rows[1].id);
  TEST_ASSERT_EQUAL_STRING("%5EGSPC", acc.rows[1].symbol);
  TEST_ASSERT_EQUAL_STRING("S&P 500", acc.rows[1].name);
  TEST_ASSERT_EQUAL_INT(SRC_YAHOO, acc.rows[1].source);
  TEST_ASSERT_EQUAL_INT(KIND_INDEX, acc.rows[1].kind);
  TEST_ASSERT_EQUAL_INT(CHG_PREV_CLOSE, acc.rows[1].change_basis);
}

// ===== multi-part assembly (parts in order) =====

static const char* P0_OF_2 =
  "{\"v\":1,\"config\":{\"rev\":3,\"part\":0,\"parts\":2,\"tickers\":["
  "{\"id\":\"a\",\"src\":\"binance\",\"sym\":\"AUSDT\",\"name\":\"A\",\"kind\":\"crypto\",\"basis\":\"24h\"}]}}";
static const char* P1_OF_2 =
  "{\"v\":1,\"config\":{\"rev\":3,\"part\":1,\"parts\":2,\"tickers\":["
  "{\"id\":\"b\",\"src\":\"yahoo\",\"sym\":\"BBB\",\"name\":\"B\",\"kind\":\"etf\",\"basis\":\"prev_close\"}]}}";

static void test_multi_part_in_order(void) {
  config_accum_t acc; memset(&acc, 0, sizeof(acc));
  const char* err;
  TEST_ASSERT_EQUAL_INT(CFG_PENDING, feed(&acc, P0_OF_2, &err));
  TEST_ASSERT_EQUAL_INT(CFG_DONE, feed(&acc, P1_OF_2, &err));
  TEST_ASSERT_EQUAL_INT(2, acc.row_count);
  TEST_ASSERT_EQUAL_STRING("a", acc.rows[0].id);   // concatenated in part order
  TEST_ASSERT_EQUAL_STRING("b", acc.rows[1].id);
}

// A new rev arriving mid-sequence (its part 0) resets the partial and starts fresh.
static void test_new_rev_resets_partial(void) {
  config_accum_t acc; memset(&acc, 0, sizeof(acc));
  const char* err;
  TEST_ASSERT_EQUAL_INT(CFG_PENDING, feed(&acc, P0_OF_2, &err));   // rev 3 partial
  const char* new_rev_p0 =
    "{\"v\":1,\"config\":{\"rev\":9,\"part\":0,\"parts\":1,\"tickers\":["
    "{\"id\":\"z\",\"src\":\"yahoo\",\"sym\":\"ZZZ\",\"name\":\"Z\",\"kind\":\"fx\",\"basis\":\"prev_close\"}]}}";
  TEST_ASSERT_EQUAL_INT(CFG_DONE, feed(&acc, new_rev_p0, &err));   // rev 9 single-part wins
  TEST_ASSERT_EQUAL_UINT32(9, acc.rev);
  TEST_ASSERT_EQUAL_INT(1, acc.row_count);
  TEST_ASSERT_EQUAL_STRING("z", acc.rows[0].id);
}

// ===== chunking errors =====

static void test_gap_part(void) {
  // part 0 then part 2 (skipping 1) of a 3-part snapshot => bad_chunking.
  const char* p2_of_3 =
    "{\"v\":1,\"config\":{\"rev\":3,\"part\":2,\"parts\":3,\"tickers\":["
    "{\"id\":\"c\",\"src\":\"yahoo\",\"sym\":\"CCC\",\"name\":\"C\",\"kind\":\"etf\",\"basis\":\"prev_close\"}]}}";
  const char* p0_of_3 =
    "{\"v\":1,\"config\":{\"rev\":3,\"part\":0,\"parts\":3,\"tickers\":["
    "{\"id\":\"a\",\"src\":\"yahoo\",\"sym\":\"AAA\",\"name\":\"A\",\"kind\":\"etf\",\"basis\":\"prev_close\"}]}}";
  config_accum_t acc; memset(&acc, 0, sizeof(acc));
  const char* err;
  TEST_ASSERT_EQUAL_INT(CFG_PENDING, feed(&acc, p0_of_3, &err));
  TEST_ASSERT_EQUAL_INT(CFG_ERR, feed(&acc, p2_of_3, &err));
  TEST_ASSERT_EQUAL_STRING("bad_chunking", err);
}

// Re-sending part 1 after the snapshot completed (no active partial, not part 0) => bad_chunking.
static void test_duplicate_part(void) {
  config_accum_t acc; memset(&acc, 0, sizeof(acc));
  const char* err;
  TEST_ASSERT_EQUAL_INT(CFG_PENDING, feed(&acc, P0_OF_2, &err));
  TEST_ASSERT_EQUAL_INT(CFG_DONE, feed(&acc, P1_OF_2, &err));
  TEST_ASSERT_EQUAL_INT(CFG_ERR, feed(&acc, P1_OF_2, &err));
  TEST_ASSERT_EQUAL_STRING("bad_chunking", err);
}

// A malformed frame where part >= parts is rejected at parse (structurally invalid) => malformed.
static void test_part_out_of_range(void) {
  const char* j =
    "{\"v\":1,\"config\":{\"rev\":1,\"part\":2,\"parts\":2,\"tickers\":["
    "{\"id\":\"a\",\"src\":\"yahoo\",\"sym\":\"AAA\",\"name\":\"A\",\"kind\":\"etf\",\"basis\":\"prev_close\"}]}}";
  config_chunk_t ch; memset(&ch, 0, sizeof(ch));
  const char* err = NULL;
  TEST_ASSERT_EQUAL_INT(ERR_PARSE, hub_parse_config_chunk(j, strlen(j), &ch, &err));
  TEST_ASSERT_EQUAL_STRING("malformed", err);
}

// ===== count / enum / length error matrix =====

// Part 0 carries MAX_TICKERS rows; part 1 adds one more => overflow on append.
static void test_too_many_tickers(void) {
  config_accum_t acc; memset(&acc, 0, sizeof(acc));
  const char* err;
  char buf0[3072];
  int off = snprintf(buf0, sizeof(buf0), "{\"v\":1,\"config\":{\"rev\":1,\"part\":0,\"parts\":2,\"tickers\":[");
  for (int i = 0; i < MAX_TICKERS; i++)
    off += snprintf(buf0 + off, sizeof(buf0) - off,
      "%s{\"id\":\"t%d\",\"src\":\"yahoo\",\"sym\":\"S%d\",\"name\":\"N%d\",\"kind\":\"etf\",\"basis\":\"prev_close\"}",
      i ? "," : "", i, i, i);
  snprintf(buf0 + off, sizeof(buf0) - off, "]}}");
  const char* p1 =
    "{\"v\":1,\"config\":{\"rev\":1,\"part\":1,\"parts\":2,\"tickers\":["
    "{\"id\":\"x\",\"src\":\"yahoo\",\"sym\":\"X\",\"name\":\"X\",\"kind\":\"etf\",\"basis\":\"prev_close\"}]}}";
  TEST_ASSERT_EQUAL_INT(CFG_PENDING, feed(&acc, buf0, &err));
  TEST_ASSERT_EQUAL_INT(CFG_ERR, feed(&acc, p1, &err));
  TEST_ASSERT_EQUAL_STRING("too_many_tickers", err);
}

static void test_empty_snapshot(void) {
  const char* j = "{\"v\":1,\"config\":{\"rev\":1,\"part\":0,\"parts\":1,\"tickers\":[]}}";
  config_accum_t acc; memset(&acc, 0, sizeof(acc));
  const char* err;
  TEST_ASSERT_EQUAL_INT(CFG_ERR, feed(&acc, j, &err));
  TEST_ASSERT_EQUAL_STRING("empty", err);
}

// Table-driven: each bad field => its specific err string at parse time.
typedef struct { const char* name; const char* json; const char* want_err; } bad_case_t;

static void test_bad_field_matrix(void) {
  static const bad_case_t cases[] = {
    {"bad_source", "{\"v\":1,\"config\":{\"rev\":1,\"part\":0,\"parts\":1,\"tickers\":["
      "{\"id\":\"a\",\"src\":\"kraken\",\"sym\":\"S\",\"name\":\"N\",\"kind\":\"crypto\",\"basis\":\"24h\"}]}}", "bad_source"},
    {"bad_kind", "{\"v\":1,\"config\":{\"rev\":1,\"part\":0,\"parts\":1,\"tickers\":["
      "{\"id\":\"a\",\"src\":\"yahoo\",\"sym\":\"S\",\"name\":\"N\",\"kind\":\"bond\",\"basis\":\"prev_close\"}]}}", "bad_kind"},
    {"bad_basis", "{\"v\":1,\"config\":{\"rev\":1,\"part\":0,\"parts\":1,\"tickers\":["
      "{\"id\":\"a\",\"src\":\"yahoo\",\"sym\":\"S\",\"name\":\"N\",\"kind\":\"etf\",\"basis\":\"weekly\"}]}}", "bad_basis"},
    {"empty_id", "{\"v\":1,\"config\":{\"rev\":1,\"part\":0,\"parts\":1,\"tickers\":["
      "{\"id\":\"\",\"src\":\"yahoo\",\"sym\":\"S\",\"name\":\"N\",\"kind\":\"etf\",\"basis\":\"prev_close\"}]}}", "malformed"},
    {"missing_id", "{\"v\":1,\"config\":{\"rev\":1,\"part\":0,\"parts\":1,\"tickers\":["
      "{\"src\":\"yahoo\",\"sym\":\"S\",\"name\":\"N\",\"kind\":\"etf\",\"basis\":\"prev_close\"}]}}", "malformed"},
    {"long_id", "{\"v\":1,\"config\":{\"rev\":1,\"part\":0,\"parts\":1,\"tickers\":["
      "{\"id\":\"0123456789abcdef0\",\"src\":\"yahoo\",\"sym\":\"S\",\"name\":\"N\",\"kind\":\"etf\",\"basis\":\"prev_close\"}]}}", "malformed"},
    {"long_sym", "{\"v\":1,\"config\":{\"rev\":1,\"part\":0,\"parts\":1,\"tickers\":["
      "{\"id\":\"a\",\"src\":\"yahoo\",\"sym\":\"0123456789012345678901234\",\"name\":\"N\",\"kind\":\"etf\",\"basis\":\"prev_close\"}]}}", "malformed"},
    {"long_name", "{\"v\":1,\"config\":{\"rev\":1,\"part\":0,\"parts\":1,\"tickers\":["
      "{\"id\":\"a\",\"src\":\"yahoo\",\"sym\":\"S\",\"name\":\"0123456789012345678901234\",\"kind\":\"etf\",\"basis\":\"prev_close\"}]}}", "malformed"},
    {"garbage", "not json", "malformed"},
    {"bad_version", "{\"v\":2,\"config\":{\"rev\":1,\"part\":0,\"parts\":1,\"tickers\":[]}}", "malformed"},
    {"no_config", "{\"v\":1,\"usage\":{}}", "malformed"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    config_chunk_t ch; memset(&ch, 0, sizeof(ch));
    const char* err = NULL;
    data_err_t pe = hub_parse_config_chunk(cases[i].json, strlen(cases[i].json), &ch, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ERR_PARSE, pe, cases[i].name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i].want_err, err, cases[i].name);
  }
}

// id/sym/name exactly at the bound (capacity-1) is accepted (boundary, not over-length).
static void test_max_length_fields_accepted(void) {
  // FIN_ID_LEN-1 = 15, TKR_SYM_LEN-1 = 23, TKR_NAME_LEN-1 = 23
  const char* j =
    "{\"v\":1,\"config\":{\"rev\":1,\"part\":0,\"parts\":1,\"tickers\":["
    "{\"id\":\"012345678901234\",\"src\":\"yahoo\","
    "\"sym\":\"01234567890123456789012\",\"name\":\"01234567890123456789012\","
    "\"kind\":\"etf\",\"basis\":\"prev_close\"}]}}";
  config_accum_t acc; memset(&acc, 0, sizeof(acc));
  const char* err;
  TEST_ASSERT_EQUAL_INT(CFG_DONE, feed(&acc, j, &err));
  TEST_ASSERT_EQUAL_size_t(FIN_ID_LEN - 1, strlen(acc.rows[0].id));
  TEST_ASSERT_EQUAL_size_t(TKR_SYM_LEN - 1, strlen(acc.rows[0].symbol));
  TEST_ASSERT_EQUAL_size_t(TKR_NAME_LEN - 1, strlen(acc.rows[0].name));
}

// ===== ack build (exact JSON) =====

static void test_ack_build_ok(void) {
  char buf[128];
  size_t n = hub_build_config_ack(buf, sizeof(buf), 7, true, NULL, 8);
  TEST_ASSERT_GREATER_THAN_size_t(0, n);
  TEST_ASSERT_EQUAL_CHAR('\n', buf[n - 1]);
  TEST_ASSERT_EQUAL_CHAR('\0', buf[n]);
  buf[n - 1] = '\0';   // strip newline for the exact compare
  TEST_ASSERT_EQUAL_STRING("{\"v\":1,\"cmd\":\"config_ack\",\"rev\":7,\"ok\":true,\"count\":8}", buf);
}

static void test_ack_build_err_matrix(void) {
  static const char* errs[] = {
    "too_many_tickers", "empty", "bad_source", "bad_kind", "bad_basis", "bad_chunking", "malformed",
  };
  for (size_t i = 0; i < sizeof(errs) / sizeof(errs[0]); i++) {
    char buf[160];
    size_t n = hub_build_config_ack(buf, sizeof(buf), 7, false, errs[i], 0);
    TEST_ASSERT_GREATER_THAN_size_t(0, n);
    buf[n - 1] = '\0';
    char want[160];
    snprintf(want, sizeof(want), "{\"v\":1,\"cmd\":\"config_ack\",\"rev\":7,\"ok\":false,\"err\":\"%s\"}", errs[i]);
    TEST_ASSERT_EQUAL_STRING(want, buf);
  }
}

static void test_ack_build_overflow(void) {
  char buf[8];   // too small for any valid ack
  TEST_ASSERT_EQUAL_size_t(0, hub_build_config_ack(buf, sizeof(buf), 7, true, NULL, 8));
}

// A representative single-part snapshot fits comfortably under HUB_FRAME_MAX (chunk budget ~900 B).
static void test_example_chunk_under_budget(void) {
  const char* j =
    "{\"v\":1,\"config\":{\"rev\":7,\"part\":0,\"parts\":2,\"tickers\":["
    "{\"id\":\"bz_btcusdt\",\"src\":\"binance\",\"sym\":\"BTCUSDT\",\"name\":\"BTC\","
    "\"kind\":\"crypto\",\"cadence\":60,\"stale\":600,\"basis\":\"24h\"},"
    "{\"id\":\"yh_gspc\",\"src\":\"yahoo\",\"sym\":\"%5EGSPC\",\"name\":\"S&P 500\","
    "\"kind\":\"index\",\"cadence\":300,\"stale\":600,\"basis\":\"prev_close\"}]}}";
  TEST_ASSERT_LESS_THAN_size_t(900, strlen(j));
  TEST_ASSERT_LESS_THAN_size_t(HUB_FRAME_MAX, strlen(j));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_single_part_valid);
  RUN_TEST(test_multi_part_in_order);
  RUN_TEST(test_new_rev_resets_partial);
  RUN_TEST(test_gap_part);
  RUN_TEST(test_duplicate_part);
  RUN_TEST(test_part_out_of_range);
  RUN_TEST(test_too_many_tickers);
  RUN_TEST(test_empty_snapshot);
  RUN_TEST(test_bad_field_matrix);
  RUN_TEST(test_max_length_fields_accepted);
  RUN_TEST(test_ack_build_ok);
  RUN_TEST(test_ack_build_err_matrix);
  RUN_TEST(test_ack_build_overflow);
  RUN_TEST(test_example_chunk_under_budget);
  return UNITY_END();
}
