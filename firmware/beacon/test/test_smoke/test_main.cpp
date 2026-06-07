#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_harness_runs(void) {
  TEST_ASSERT_EQUAL_INT(4, 2 + 2);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_harness_runs);
  return UNITY_END();
}
