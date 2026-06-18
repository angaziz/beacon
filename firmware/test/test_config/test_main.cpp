#include <unity.h>
#include <string.h>
#include "config/tickers.h"
#include "config/location.h"
#include "core/records.h"   // FIN_ID_LEN: ticker id must fit finance_rec_t.id

void setUp(void) {}
void tearDown(void) {}

static void test_tickers_fit(void) {
  TEST_ASSERT_TRUE(DEFAULT_TICKERS_COUNT > 0);
  TEST_ASSERT_TRUE(DEFAULT_TICKERS_COUNT <= MAX_TICKERS);
}

static void test_ticker_ids_unique(void) {
  for (uint8_t i = 0; i < DEFAULT_TICKERS_COUNT; i++) {
    TEST_ASSERT_NOT_NULL(DEFAULT_TICKERS[i].id);
    TEST_ASSERT_TRUE(strlen(DEFAULT_TICKERS[i].id) < FIN_ID_LEN);  // must fit finance_rec_t.id (+NUL)
    for (uint8_t j = i + 1; j < DEFAULT_TICKERS_COUNT; j++) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(DEFAULT_TICKERS[i].id, DEFAULT_TICKERS[j].id) != 0,
                               "duplicate ticker id");
    }
  }
}

// every enum in range + sane cadence/stale (table-driven over the whole config)
static void test_ticker_fields_valid(void) {
  for (uint8_t i = 0; i < DEFAULT_TICKERS_COUNT; i++) {
    const ticker_cfg_t* t = &DEFAULT_TICKERS[i];
    TEST_ASSERT_TRUE(t->source >= SRC_BINANCE && t->source <= SRC_YAHOO);
    TEST_ASSERT_TRUE(t->kind >= KIND_FX && t->kind <= KIND_ETF);
    TEST_ASSERT_TRUE(t->change_basis == CHG_PREV_CLOSE || t->change_basis == CHG_24H);
    TEST_ASSERT_NOT_NULL(t->symbol);
    TEST_ASSERT_NOT_NULL(t->display_name);
    TEST_ASSERT_TRUE(t->cadence_s > 0);
    TEST_ASSERT_TRUE(t->stale_s >= t->cadence_s);  // stale threshold must trail the fetch period
  }
}

static void test_wmo_map(void) {
  TEST_ASSERT_TRUE(WMO_MAP_COUNT > 0);
  for (uint16_t i = 0; i < WMO_MAP_COUNT; i++) {
    TEST_ASSERT_NOT_NULL(WMO_MAP[i].label);
    TEST_ASSERT_NOT_NULL(WMO_MAP[i].icon);
    TEST_ASSERT_TRUE(strlen(WMO_MAP[i].label) > 0);
  }
}

static void test_location_default(void) {
  TEST_ASSERT_NOT_NULL(DEFAULT_LOCATION.tz_id);
  TEST_ASSERT_NOT_NULL(DEFAULT_LOCATION.ntp_server);
  TEST_ASSERT_EQUAL_STRING("Asia/Jakarta", DEFAULT_LOCATION.tz_id);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_tickers_fit);
  RUN_TEST(test_ticker_ids_unique);
  RUN_TEST(test_ticker_fields_valid);
  RUN_TEST(test_wmo_map);
  RUN_TEST(test_location_default);
  return UNITY_END();
}
