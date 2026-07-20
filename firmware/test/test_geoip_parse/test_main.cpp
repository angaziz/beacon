#include <unity.h>
#include <string.h>
#include "fetch/parse_geoip.h"

void setUp(void) {}
void tearDown(void) {}

static void test_ipwhois_ok(void) {
  const char* j = "{\"ip\":\"1.2.3.4\",\"success\":true,\"city\":\"Mission\",\"region\":\"California\","
                  "\"latitude\":37.7749,\"longitude\":-122.4194,"
                  "\"timezone\":{\"id\":\"America/Los_Angeles\",\"abbr\":\"PST\",\"offset\":-28800}}";
  float lat = 0, lon = 0; char tz[40] = ""; char city[40] = ""; char region[40] = "";
  TEST_ASSERT_EQUAL(ERR_NONE, parse_geoip(j, strlen(j), &lat, &lon, tz, sizeof(tz), city, sizeof(city), region, sizeof(region)));
  TEST_ASSERT_FLOAT_WITHIN(0.001, 37.7749f, lat);
  TEST_ASSERT_FLOAT_WITHIN(0.001, -122.4194f, lon);
  TEST_ASSERT_EQUAL_STRING("America/Los_Angeles", tz);
  TEST_ASSERT_EQUAL_STRING("Mission", city);
  TEST_ASSERT_EQUAL_STRING("California", region);
}

static void test_success_false_is_parse_err(void) {
  const char* j = "{\"success\":false,\"message\":\"Invalid IP address\"}";
  float lat = 0, lon = 0; char tz[40] = "";
  TEST_ASSERT_EQUAL(ERR_PARSE, parse_geoip(j, strlen(j), &lat, &lon, tz, sizeof(tz), NULL, 0, NULL, 0));
}

static void test_missing_coords_is_parse_err(void) {
  const char* j = "{\"success\":true,\"city\":\"Nowhere\"}";
  float lat = 0, lon = 0; char tz[40] = "";
  TEST_ASSERT_EQUAL(ERR_PARSE, parse_geoip(j, strlen(j), &lat, &lon, tz, sizeof(tz), NULL, 0, NULL, 0));
}

static void test_missing_timezone_ok_empty(void) {
  const char* j = "{\"success\":true,\"latitude\":1.5,\"longitude\":2.5}";
  float lat = 0, lon = 0; char tz[40] = "x";
  TEST_ASSERT_EQUAL(ERR_NONE, parse_geoip(j, strlen(j), &lat, &lon, tz, sizeof(tz), NULL, 0, NULL, 0));
  TEST_ASSERT_EQUAL_STRING("", tz);   // tz absent => empty, caller keeps current zone
}

static void test_bdc_city_picks_city_level(void) {
  const char* j = "{\"localityInfo\":{\"administrative\":["
    "{\"name\":\"United States\",\"description\":\"country in North America\"},"
    "{\"name\":\"California\",\"description\":\"province in United States\"},"
    "{\"name\":\"San Francisco\",\"description\":\"city in California, United States\"},"
    "{\"name\":\"Mission District\",\"description\":\"district in San Francisco\"}]}}";
  char city[40] = "";
  TEST_ASSERT_EQUAL(ERR_NONE, parse_bdc_city(j, strlen(j), city, sizeof(city)));
  TEST_ASSERT_EQUAL_STRING("San Francisco", city);   // not the province, district, or country
}

static void test_bdc_city_none(void) {
  const char* j = "{\"localityInfo\":{\"administrative\":["
    "{\"name\":\"United States\",\"description\":\"country in North America\"}]}}";
  char city[40] = "";
  TEST_ASSERT_EQUAL(ERR_PARSE, parse_bdc_city(j, strlen(j), city, sizeof(city)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_bdc_city_picks_city_level);
  RUN_TEST(test_bdc_city_none);
  RUN_TEST(test_ipwhois_ok);
  RUN_TEST(test_success_false_is_parse_err);
  RUN_TEST(test_missing_coords_is_parse_err);
  RUN_TEST(test_missing_timezone_ok_empty);
  return UNITY_END();
}
