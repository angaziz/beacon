#include <unity.h>
#include "core/idle_policy.h"

void setUp(void) {} void tearDown(void) {}

// dim at 30s, sleep at 300s (the ui/input.cpp device constants).
static void test_thresholds(void) {
  struct { uint32_t inactive, dim, sleep; disp_power_t expect; } c[] = {
    {0,      30000, 300000, DISP_AWAKE},
    {29999,  30000, 300000, DISP_AWAKE},
    {30000,  30000, 300000, DISP_DIM},     // dim boundary is inclusive
    {299999, 30000, 300000, DISP_DIM},
    {300000, 30000, 300000, DISP_ASLEEP},  // sleep boundary is inclusive
    {999999, 30000, 300000, DISP_ASLEEP},
  };
  for (size_t i = 0; i < sizeof(c)/sizeof(c[0]); i++)
    TEST_ASSERT_EQUAL_INT(c[i].expect, idle_power_for(c[i].inactive, c[i].dim, c[i].sleep));
}
int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_thresholds);
  return UNITY_END();
}
