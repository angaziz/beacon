#include <unity.h>
#include <string.h>
#include "fetch/parse_finance.h"

void setUp(void) {}
void tearDown(void) {}

static void test_binance_string_numbers(void) {
  const char* j = "{\"symbol\":\"BTCUSDT\",\"lastPrice\":\"62392.10\",\"priceChangePercent\":\"2.14\"}";
  double last = 0, chg = 0;
  TEST_ASSERT_EQUAL(ERR_NONE, parse_binance(j, strlen(j), &last, &chg));
  TEST_ASSERT_DOUBLE_WITHIN(0.001, 62392.10, last);
  TEST_ASSERT_DOUBLE_WITHIN(0.001, 2.14, chg);
}

static void test_binance_missing_price(void) {
  const char* j = "{\"symbol\":\"BTCUSDT\"}";
  double last = 0, chg = 0;
  TEST_ASSERT_EQUAL(ERR_PARSE, parse_binance(j, strlen(j), &last, &chg));
}

static void test_frankfurter_rate(void) {
  const char* j = "{\"amount\":1,\"base\":\"USD\",\"date\":\"2026-06-05\",\"rates\":{\"IDR\":16320.5}}";
  double rate = 0;
  TEST_ASSERT_EQUAL(ERR_NONE, parse_frankfurter(j, strlen(j), &rate));
  TEST_ASSERT_DOUBLE_WITHIN(0.001, 16320.5, rate);
}

static void test_frankfurter_missing_idr(void) {
  const char* j = "{\"base\":\"USD\",\"rates\":{\"EUR\":0.92}}";
  double rate = 0;
  TEST_ASSERT_EQUAL(ERR_PARSE, parse_frankfurter(j, strlen(j), &rate));
}

static void test_frankfurter_series(void) {
  const char* j = "{\"amount\":1.0,\"base\":\"USD\",\"start_date\":\"2026-06-01\",\"end_date\":\"2026-06-05\","
                  "\"rates\":{\"2026-06-03\":{\"IDR\":17962},\"2026-06-04\":{\"IDR\":18026},"
                  "\"2026-06-05\":{\"IDR\":18070}}}";
  double latest = 0, prev = 0;
  TEST_ASSERT_EQUAL(ERR_NONE, parse_frankfurter_series(j, strlen(j), &latest, &prev));
  TEST_ASSERT_DOUBLE_WITHIN(0.001, 18070, latest);   // most recent day
  TEST_ASSERT_DOUBLE_WITHIN(0.001, 18026, prev);     // prior business day (real prev-close)
}

static void test_frankfurter_series_single_day(void) {
  const char* j = "{\"rates\":{\"2026-06-05\":{\"IDR\":18070}}}";
  double latest = 0, prev = 0;
  TEST_ASSERT_EQUAL(ERR_NONE, parse_frankfurter_series(j, strlen(j), &latest, &prev));
  TEST_ASSERT_DOUBLE_WITHIN(0.001, 18070, latest);
  TEST_ASSERT_DOUBLE_WITHIN(0.001, 18070, prev);     // one day => prev==latest => 0 change
}

static void test_yahoo_meta(void) {
  const char* j = "{\"chart\":{\"result\":[{\"meta\":{\"symbol\":\"^GSPC\","
                  "\"regularMarketPrice\":5594.1,\"previousClose\":5570.2,\"chartPreviousClose\":5560.0}}],"
                  "\"error\":null}}";
  double price = 0, prev = 0;
  TEST_ASSERT_EQUAL(ERR_NONE, parse_yahoo(j, strlen(j), &price, &prev));
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 5594.1, price);
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 5570.2, prev);   // previousClose preferred over chartPreviousClose
}

static void test_yahoo_prevclose_fallback(void) {
  const char* j = "{\"chart\":{\"result\":[{\"meta\":{\"regularMarketPrice\":100.0,"
                  "\"chartPreviousClose\":98.0}}]}}";
  double price = 0, prev = 0;
  TEST_ASSERT_EQUAL(ERR_NONE, parse_yahoo(j, strlen(j), &price, &prev));
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 98.0, prev);     // falls back to chartPreviousClose
}

static void test_yahoo_error_payload(void) {
  const char* j = "{\"chart\":{\"result\":null,\"error\":{\"code\":\"Not Found\"}}}";
  double price = 0, prev = 0;
  TEST_ASSERT_EQUAL(ERR_PARSE, parse_yahoo(j, strlen(j), &price, &prev));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_binance_string_numbers);
  RUN_TEST(test_binance_missing_price);
  RUN_TEST(test_frankfurter_rate);
  RUN_TEST(test_frankfurter_missing_idr);
  RUN_TEST(test_yahoo_meta);
  RUN_TEST(test_yahoo_prevclose_fallback);
  RUN_TEST(test_yahoo_error_payload);
  return UNITY_END();
}
