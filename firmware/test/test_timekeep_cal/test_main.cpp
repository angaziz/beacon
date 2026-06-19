#include <unity.h>
#include "core/timekeep_cal.h"

void setUp(void) {}
void tearDown(void) {}

static void test_unix_epoch(void) {
  // 1970-01-01 00:00:00 UTC == epoch 0
  TEST_ASSERT_EQUAL_INT64(0, utc_to_epoch(1970, 1, 1, 0, 0, 0));
}

static void test_known_dates(void) {
  // 2000-01-01 00:00:00 UTC == 946684800
  TEST_ASSERT_EQUAL_INT64(946684800, utc_to_epoch(2000, 1, 1, 0, 0, 0));
  // 2024-01-01 00:00:00 UTC == 1704067200
  TEST_ASSERT_EQUAL_INT64(1704067200, utc_to_epoch(2024, 1, 1, 0, 0, 0));
  // 2026-06-11 12:00:00 UTC == 1781179200
  TEST_ASSERT_EQUAL_INT64(1781179200, utc_to_epoch(2026, 6, 11, 12, 0, 0));
}

static void test_leap_year_feb29(void) {
  // 2024-02-29 00:00:00 UTC == 1709164800
  TEST_ASSERT_EQUAL_INT64(1709164800, utc_to_epoch(2024, 2, 29, 0, 0, 0));
  // 2000-02-29 00:00:00 UTC == 951782400
  TEST_ASSERT_EQUAL_INT64(951782400, utc_to_epoch(2000, 2, 29, 0, 0, 0));
}

static void test_end_of_day(void) {
  // 2024-12-31 23:59:59 UTC == 1735689599
  TEST_ASSERT_EQUAL_INT64(1735689599, utc_to_epoch(2024, 12, 31, 23, 59, 59));
}

static void test_time_components(void) {
  // Base: 2024-06-15 00:00:00 = 1718409600
  time_t base = utc_to_epoch(2024, 6, 15, 0, 0, 0);
  TEST_ASSERT_EQUAL_INT64(1718409600, base);
  // + 1 hour
  TEST_ASSERT_EQUAL_INT64(base + 3600, utc_to_epoch(2024, 6, 15, 1, 0, 0));
  // + 1 minute
  TEST_ASSERT_EQUAL_INT64(base + 60, utc_to_epoch(2024, 6, 15, 0, 1, 0));
  // + 1 second
  TEST_ASSERT_EQUAL_INT64(base + 1, utc_to_epoch(2024, 6, 15, 0, 0, 1));
  // combined: 13:45:30
  TEST_ASSERT_EQUAL_INT64(base + 13*3600 + 45*60 + 30, utc_to_epoch(2024, 6, 15, 13, 45, 30));
}

static void test_month_boundaries(void) {
  // Jan 31 -> Feb 1
  time_t jan31 = utc_to_epoch(2024, 1, 31, 0, 0, 0);
  time_t feb01 = utc_to_epoch(2024, 2, 1, 0, 0, 0);
  TEST_ASSERT_EQUAL_INT64(86400, feb01 - jan31);

  // Feb 28 -> Feb 29 (leap year 2024)
  time_t feb28 = utc_to_epoch(2024, 2, 28, 0, 0, 0);
  time_t feb29 = utc_to_epoch(2024, 2, 29, 0, 0, 0);
  TEST_ASSERT_EQUAL_INT64(86400, feb29 - feb28);

  // Feb 29 -> Mar 1 (leap year 2024)
  time_t mar01 = utc_to_epoch(2024, 3, 1, 0, 0, 0);
  TEST_ASSERT_EQUAL_INT64(86400, mar01 - feb29);
}

static void test_non_leap_year(void) {
  // 2023 is not a leap year: Feb 28 -> Mar 1 = 1 day
  time_t feb28 = utc_to_epoch(2023, 2, 28, 0, 0, 0);
  time_t mar01 = utc_to_epoch(2023, 3, 1, 0, 0, 0);
  TEST_ASSERT_EQUAL_INT64(86400, mar01 - feb28);
}

static void test_year_boundary(void) {
  // 2023-12-31 23:59:59 -> 2024-01-01 00:00:00
  time_t dec31 = utc_to_epoch(2023, 12, 31, 23, 59, 59);
  time_t jan01 = utc_to_epoch(2024, 1, 1, 0, 0, 0);
  TEST_ASSERT_EQUAL_INT64(1, jan01 - dec31);
}

static void test_century_leap(void) {
  // 1900 is NOT a leap year (divisible by 100 but not 400)
  time_t feb28_1900 = utc_to_epoch(1900, 2, 28, 0, 0, 0);
  time_t mar01_1900 = utc_to_epoch(1900, 3, 1, 0, 0, 0);
  TEST_ASSERT_EQUAL_INT64(86400, mar01_1900 - feb28_1900);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_unix_epoch);
  RUN_TEST(test_known_dates);
  RUN_TEST(test_leap_year_feb29);
  RUN_TEST(test_end_of_day);
  RUN_TEST(test_time_components);
  RUN_TEST(test_month_boundaries);
  RUN_TEST(test_non_leap_year);
  RUN_TEST(test_year_boundary);
  RUN_TEST(test_century_leap);
  return UNITY_END();
}
