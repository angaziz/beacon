#include <unity.h>
#include "config/ticker_table.h"
#include "core/stale.h"

void setUp(void) { ticker_table_init(); }
void tearDown(void) {}

static void test_constants(void) {
  TEST_ASSERT_EQUAL_UINT32(1800, WEATHER_STALE_S);
  TEST_ASSERT_EQUAL_UINT32(300, USAGE_STALE_S);
  TEST_ASSERT_EQUAL_UINT32(300, BUDDY_STALE_S);
}

static void test_finance_stale_from_runtime_table(void) {
  // ticker_table starts with defaults; slot 0 should have a valid stale_s > 0.
  uint32_t s = finance_stale_s(0);
  TEST_ASSERT_GREATER_THAN_UINT32(0, s);
}

static void test_finance_stale_invalid_index_zero(void) {
  // An index beyond the table's row count should return 0 (ticker_table_get returns false).
  uint32_t s = finance_stale_s(255);
  TEST_ASSERT_EQUAL_UINT32(0, s);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_constants);
  RUN_TEST(test_finance_stale_from_runtime_table);
  RUN_TEST(test_finance_stale_invalid_index_zero);
  return UNITY_END();
}
