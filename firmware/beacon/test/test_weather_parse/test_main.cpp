#include <unity.h>
#include <string.h>
#include "fetch/parse_weather.h"

void setUp(void) {}
void tearDown(void) {}

static void test_open_meteo_ok(void) {
  const char* j = "{\"latitude\":-6.2,\"longitude\":106.8,"
                  "\"current\":{\"time\":\"2026-06-08T05:00\",\"temperature_2m\":31.8,"
                  "\"relative_humidity_2m\":57,\"weather_code\":2}}";
  weather_rec_t w; memset(&w, 0, sizeof(w));
  TEST_ASSERT_EQUAL(ERR_NONE, parse_weather(j, strlen(j), &w));
  TEST_ASSERT_FLOAT_WITHIN(0.01, 31.8f, w.temp_c);
  TEST_ASSERT_FLOAT_WITHIN(0.01, 57.0f, w.humidity_pct);
  TEST_ASSERT_EQUAL_UINT16(2, w.wmo_code);
}

static void test_missing_current_is_parse_err(void) {
  const char* j = "{\"latitude\":-6.2}";
  weather_rec_t w; memset(&w, 0, sizeof(w));
  TEST_ASSERT_EQUAL(ERR_PARSE, parse_weather(j, strlen(j), &w));
}

static void test_garbage_is_parse_err(void) {
  const char* j = "not json";
  weather_rec_t w; memset(&w, 0, sizeof(w));
  TEST_ASSERT_EQUAL(ERR_PARSE, parse_weather(j, strlen(j), &w));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_open_meteo_ok);
  RUN_TEST(test_missing_current_is_parse_err);
  RUN_TEST(test_garbage_is_parse_err);
  return UNITY_END();
}
