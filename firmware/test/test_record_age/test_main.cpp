#include <unity.h>
#include "core/records.h"

void setUp(void) {}
void tearDown(void) {}

static void test_age_never_updated(void) {
  record_hdr_t h = {0, ST_LOADING, ERR_NONE};
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, record_age_s(&h, 1000));
}

static void test_age_zero(void) {
  record_hdr_t h = {500, ST_LIVE, ERR_NONE};
  TEST_ASSERT_EQUAL_UINT32(0, record_age_s(&h, 500));
}

static void test_age_positive(void) {
  record_hdr_t h = {1000, ST_LIVE, ERR_NONE};
  TEST_ASSERT_EQUAL_UINT32(200, record_age_s(&h, 1200));
}

static void test_age_large(void) {
  record_hdr_t h = {100, ST_STALE, ERR_NONE};
  TEST_ASSERT_EQUAL_UINT32(86300, record_age_s(&h, 86400));
}

static void test_state_enum_values(void) {
  TEST_ASSERT_EQUAL_INT(0, ST_LOADING);
  TEST_ASSERT_EQUAL_INT(1, ST_LIVE);
  TEST_ASSERT_EQUAL_INT(2, ST_STALE);
  TEST_ASSERT_EQUAL_INT(3, ST_OFFLINE);
  TEST_ASSERT_EQUAL_INT(4, ST_ERROR);
  TEST_ASSERT_EQUAL_INT(5, ST_HUB_OFFLINE);
}

static void test_error_enum_values(void) {
  TEST_ASSERT_EQUAL_INT(0, ERR_NONE);
  TEST_ASSERT_EQUAL_INT(1, ERR_TIMEOUT);
  TEST_ASSERT_EQUAL_INT(2, ERR_HTTP);
  TEST_ASSERT_EQUAL_INT(3, ERR_RATE_LIMITED);
  TEST_ASSERT_EQUAL_INT(4, ERR_PARSE);
  TEST_ASSERT_EQUAL_INT(5, ERR_NO_ROUTE);
}

static void test_string_capacities(void) {
  TEST_ASSERT_EQUAL(16, FIN_ID_LEN);
  TEST_ASSERT_EQUAL(24, BUDDY_ID_LEN);
  TEST_ASSERT_EQUAL(24, BUDDY_TOOL_LEN);
  TEST_ASSERT_EQUAL(80, BUDDY_HINT_LEN);
  TEST_ASSERT_EQUAL(40, BUDDY_ENTRY_LEN);
  TEST_ASSERT_EQUAL(3,  BUDDY_ENTRIES);
}

static void test_prompt_lifecycle_constants(void) {
  TEST_ASSERT_EQUAL_UINT8(0, PROMPT_IDLE_DECISION);
  TEST_ASSERT_EQUAL_UINT8(1, PROMPT_PENDING);
  TEST_ASSERT_EQUAL_UINT8(2, PROMPT_SENT_OK);
  TEST_ASSERT_EQUAL_UINT8(3, PROMPT_TOO_LATE);
  TEST_ASSERT_EQUAL_UINT32(180, BUDDY_PROMPT_EXPIRY_S);
  TEST_ASSERT_EQUAL_UINT32(2, BUDDY_CONFIRM_HOLD_S);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_age_never_updated);
  RUN_TEST(test_age_zero);
  RUN_TEST(test_age_positive);
  RUN_TEST(test_age_large);
  RUN_TEST(test_state_enum_values);
  RUN_TEST(test_error_enum_values);
  RUN_TEST(test_string_capacities);
  RUN_TEST(test_prompt_lifecycle_constants);
  return UNITY_END();
}
