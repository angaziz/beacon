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
  RUN_TEST(test_yahoo_meta);
  RUN_TEST(test_yahoo_prevclose_fallback);
  RUN_TEST(test_yahoo_error_payload);
  return UNITY_END();
}
