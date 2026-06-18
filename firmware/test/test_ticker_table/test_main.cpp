#include <unity.h>
#include <string.h>
#include "config/ticker_table.h"
#include "config/tickers.h"

void setUp(void) { ticker_table_init(); }   // each test starts from a freshly seeded table
void tearDown(void) {}

static void test_count_matches_defaults(void) {
  TEST_ASSERT_EQUAL_INT((int)DEFAULT_TICKERS_COUNT, ticker_table_count());
}

static void test_row0_matches_defaults(void) {
  ticker_runtime_t t;
  TEST_ASSERT_TRUE(ticker_table_get(0, &t));
  const ticker_cfg_t* d = &DEFAULT_TICKERS[0];
  TEST_ASSERT_EQUAL_STRING(d->id, t.id);
  TEST_ASSERT_EQUAL_STRING(d->symbol, t.symbol);
  TEST_ASSERT_EQUAL_STRING(d->display_name, t.name);
  TEST_ASSERT_EQUAL_INT(d->source, t.source);
  TEST_ASSERT_EQUAL_INT(d->kind, t.kind);
  TEST_ASSERT_EQUAL_UINT16(d->cadence_s, t.cadence_s);
  TEST_ASSERT_EQUAL_UINT32(d->stale_s, t.stale_s);
  TEST_ASSERT_EQUAL_INT(d->change_basis, t.change_basis);
}

static void test_get_out_of_range(void) {
  ticker_runtime_t t;
  TEST_ASSERT_FALSE(ticker_table_get(-1, &t));
  TEST_ASSERT_FALSE(ticker_table_get(ticker_table_count(), &t));
  TEST_ASSERT_FALSE(ticker_table_get(MAX_TICKERS, &t));
}

static void test_get_returns_copy(void) {
  ticker_runtime_t a;
  TEST_ASSERT_TRUE(ticker_table_get(0, &a));
  strncpy(a.name, "MUTATED", TKR_NAME_LEN - 1);   // mutate the caller's copy
  a.cadence_s = 9999;
  ticker_runtime_t b;
  TEST_ASSERT_TRUE(ticker_table_get(0, &b));       // a fresh fetch is unaffected
  TEST_ASSERT_EQUAL_STRING(DEFAULT_TICKERS[0].display_name, b.name);
  TEST_ASSERT_EQUAL_UINT16(DEFAULT_TICKERS[0].cadence_s, b.cadence_s);
}

static void test_gen_starts_zero(void) {
  TEST_ASSERT_EQUAL_UINT32(0u, ticker_table_gen());
}

static void mk_row(ticker_runtime_t* r, const char* id, const char* name) {
  memset(r, 0, sizeof(*r));
  strncpy(r->id, id, FIN_ID_LEN - 1);
  strncpy(r->name, name, TKR_NAME_LEN - 1);
  r->source = SRC_YAHOO; r->kind = KIND_INDEX; r->change_basis = CHG_PREV_CLOSE;
  r->cadence_s = 300; r->stale_s = 600;
}

// ticker_table_set swaps the table in RAM and bumps gen; accessors reflect the new rows/count.
static void test_set_swaps_and_bumps_gen(void) {
  uint32_t g0 = ticker_table_gen();
  ticker_runtime_t rows[2];
  mk_row(&rows[0], "x1", "ONE");
  mk_row(&rows[1], "x2", "TWO");
  ticker_table_set(rows, 2);
  TEST_ASSERT_EQUAL_UINT32(g0 + 1, ticker_table_gen());
  TEST_ASSERT_EQUAL_INT(2, ticker_table_count());
  ticker_runtime_t t;
  TEST_ASSERT_TRUE(ticker_table_get(1, &t));
  TEST_ASSERT_EQUAL_STRING("x2", t.id);
  TEST_ASSERT_EQUAL_STRING("TWO", t.name);
}

// Reject path: the native ticker_store_save stub returns false, so apply must NOT swap and NOT bump gen.
static void test_apply_reject_leaves_table_intact(void) {
  int     count0 = ticker_table_count();
  uint32_t gen0  = ticker_table_gen();
  ticker_runtime_t r; mk_row(&r, "zz", "ZZ");
  TEST_ASSERT_FALSE(ticker_table_apply(&r, 1));   // save stub fails on the host
  TEST_ASSERT_EQUAL_INT(count0, ticker_table_count());
  TEST_ASSERT_EQUAL_UINT32(gen0, ticker_table_gen());
  ticker_runtime_t t;
  TEST_ASSERT_TRUE(ticker_table_get(0, &t));
  TEST_ASSERT_EQUAL_STRING(DEFAULT_TICKERS[0].id, t.id);   // still the seeded defaults
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_count_matches_defaults);
  RUN_TEST(test_row0_matches_defaults);
  RUN_TEST(test_get_out_of_range);
  RUN_TEST(test_get_returns_copy);
  RUN_TEST(test_gen_starts_zero);
  RUN_TEST(test_set_swaps_and_bumps_gen);
  RUN_TEST(test_apply_reject_leaves_table_intact);
  return UNITY_END();
}
