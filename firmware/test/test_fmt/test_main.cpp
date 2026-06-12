#include <unity.h>
#include <string.h>
#include "ui/fmt.h"

void setUp(void) {} void tearDown(void) {}

static void test_fmt_value(void) {
  char b[40];
  fmt_value(b, sizeof(b), 18026);   TEST_ASSERT_EQUAL_STRING("18,026", b);
  fmt_value(b, sizeof(b), 62392);   TEST_ASSERT_EQUAL_STRING("62,392", b);
  fmt_value(b, sizeof(b), 6012);    TEST_ASSERT_EQUAL_STRING("6,012", b);
  fmt_value(b, sizeof(b), 52.18);   TEST_ASSERT_EQUAL_STRING("52.18", b);
  fmt_value(b, sizeof(b), 1.0856);  TEST_ASSERT_EQUAL_STRING("1.0856", b);  // FX majors keep 4 decimals
  fmt_value(b, sizeof(b), 157.43);  TEST_ASSERT_EQUAL_STRING("157.43", b);
  fmt_value(b, sizeof(b), 5594);    TEST_ASSERT_EQUAL_STRING("5,594", b);
  fmt_value(b, sizeof(b), 1000000); TEST_ASSERT_EQUAL_STRING("1,000,000", b);
}
static void test_fmt_change(void) {
  char b[16];
  TEST_ASSERT_EQUAL_INT(1, fmt_change(b, sizeof(b), 0.12));  TEST_ASSERT_EQUAL_STRING("^ 0.12%", b);
  TEST_ASSERT_EQUAL_INT(-1, fmt_change(b, sizeof(b), -0.88));TEST_ASSERT_EQUAL_STRING("v 0.88%", b);
  TEST_ASSERT_EQUAL_INT(0, fmt_change(b, sizeof(b), 0.0));   TEST_ASSERT_EQUAL_STRING("- 0.00%", b);
}
int main(int, char**) { UNITY_BEGIN(); RUN_TEST(test_fmt_value); RUN_TEST(test_fmt_change); return UNITY_END(); }
