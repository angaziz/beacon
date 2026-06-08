#include <unity.h>
#include "ui/carousel_nav.h"

void setUp(void) {} void tearDown(void) {}

static void test_clamp_no_wrap(void) {
  TEST_ASSERT_EQUAL_INT(0, carousel_prev(0, 6));   // clamp at start
  TEST_ASSERT_EQUAL_INT(5, carousel_next(5, 6));   // clamp at end
  TEST_ASSERT_EQUAL_INT(3, carousel_next(2, 6));
  TEST_ASSERT_EQUAL_INT(1, carousel_prev(2, 6));
  TEST_ASSERT_EQUAL_INT(0, carousel_clamp(-3, 6));
  TEST_ASSERT_EQUAL_INT(5, carousel_clamp(99, 6));
  TEST_ASSERT_EQUAL_INT(0, carousel_clamp(0, 1));  // single screen edge
}
static void test_index_for_x(void) {
  struct { int x, w, count, expect; } c[] = {
    {0, 466, 6, 0}, {232, 466, 6, 0}, {233, 466, 6, 1},
    {466, 466, 6, 1}, {466*5, 466, 6, 5}, {466*99, 466, 6, 5},
  };
  for (size_t i = 0; i < sizeof(c)/sizeof(c[0]); i++)
    TEST_ASSERT_EQUAL_INT(c[i].expect, carousel_index_for_x(c[i].x, c[i].w, c[i].count));
}
int main(int, char**) { UNITY_BEGIN(); RUN_TEST(test_clamp_no_wrap); RUN_TEST(test_index_for_x); return UNITY_END(); }
