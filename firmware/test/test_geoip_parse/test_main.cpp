#include <unity.h>
#include <string.h>
#include "fetch/parse_geoip.h"

void setUp(void) {}
void tearDown(void) {}

static void test_ipwhois_ok(void) {
  const char* j = "{\"ip\":\"1.2.3.4\",\"success\":true,\"city\":\"Suka Asih\",\"region\":\"Jawa Barat\","
                  "\"latitude\":-6.9329255,\"longitude\":107.5875009,"
                  "\"timezone\":{\"id\":\"Asia/Jakarta\",\"abbr\":\"WIB\",\"offset\":25200}}";
  float lat = 0, lon = 0; char tz[40] = ""; char city[40] = ""; char region[40] = "";
  TEST_ASSERT_EQUAL(ERR_NONE, parse_geoip(j, strlen(j), &lat, &lon, tz, sizeof(tz), city, sizeof(city), region, sizeof(region)));
  TEST_ASSERT_FLOAT_WITHIN(0.001, -6.9329255f, lat);
  TEST_ASSERT_FLOAT_WITHIN(0.001, 107.5875009f, lon);
  TEST_ASSERT_EQUAL_STRING("Asia/Jakarta", tz);
  TEST_ASSERT_EQUAL_STRING("Suka Asih", city);
  TEST_ASSERT_EQUAL_STRING("Jawa Barat", region);
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
    "{\"name\":\"Indonesia\",\"description\":\"republic in Southeast Asia\"},"
    "{\"name\":\"Jawa Barat\",\"description\":\"province in Indonesia\"},"
    "{\"name\":\"Bandung\",\"description\":\"city in West Java Province, Indonesia\"},"
    "{\"name\":\"Kecamatan Bojongloa Kaler\",\"description\":\"district in Bandung City\"}]}}";
  char city[40] = "";
  TEST_ASSERT_EQUAL(ERR_NONE, parse_bdc_city(j, strlen(j), city, sizeof(city)));
  TEST_ASSERT_EQUAL_STRING("Bandung", city);   // not the province, district, or country
}

static void test_bdc_city_none(void) {
  const char* j = "{\"localityInfo\":{\"administrative\":["
    "{\"name\":\"Indonesia\",\"description\":\"republic in Southeast Asia\"}]}}";
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
