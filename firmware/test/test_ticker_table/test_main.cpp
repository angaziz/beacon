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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_count_matches_defaults);
  RUN_TEST(test_row0_matches_defaults);
  RUN_TEST(test_get_out_of_range);
  RUN_TEST(test_get_returns_copy);
  RUN_TEST(test_gen_starts_zero);
  return UNITY_END();
}
