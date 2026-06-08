#include <unity.h>
#include <string.h>
#include "core/tz_map.h"
#include "config/location.h"

void setUp(void) {}
void tearDown(void) {}

// Table-driven: each supported IANA id maps to the expected POSIX TZ string.
static void test_known_zones(void) {
  struct { const char* iana; const char* posix; } cases[] = {
    {"Asia/Jakarta",        "WIB-7"},
    {"Asia/Singapore",      "<+08>-8"},
    {"Asia/Tokyo",          "JST-9"},
    {"America/New_York",    "EST5EDT,M3.2.0,M11.1.0"},
    {"UTC",                 "UTC0"},
  };
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const char* got = tz_iana_to_posix(cases[i].iana);
    TEST_ASSERT_NOT_NULL_MESSAGE(got, cases[i].iana);
    TEST_ASSERT_EQUAL_STRING(cases[i].posix, got);
  }
}

static void test_unknown_returns_null(void) {
  TEST_ASSERT_NULL(tz_iana_to_posix("Mars/Olympus"));
  TEST_ASSERT_NULL(tz_iana_to_posix(""));
  TEST_ASSERT_NULL(tz_iana_to_posix(NULL));
}

// The default location's tz_id must be a zone the map actually supports (else time would fall to UTC).
static void test_default_location_is_supported(void) {
  TEST_ASSERT_NOT_NULL(tz_iana_to_posix(DEFAULT_LOCATION.tz_id));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_known_zones);
  RUN_TEST(test_unknown_returns_null);
  RUN_TEST(test_default_location_is_supported);
  return UNITY_END();
}
