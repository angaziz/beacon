#include <unity.h>
#include "core/change_basis.h"

void setUp(void) {}
void tearDown(void) {}

static void test_change_table(void) {
  struct { double value, basis, exp_change, exp_pct; } cases[] = {
    {110.0, 100.0,  10.0,  10.0},
    { 90.0, 100.0, -10.0, -10.0},
    {100.0, 100.0,   0.0,   0.0},
    {  0.0,   0.0,   0.0,   0.0},   // zero basis => pct 0, no div-by-zero
    {16320.0, 16000.0, 320.0, 2.0},
  };
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    double c = 999, p = 999;
    change_compute(cases[i].value, cases[i].basis, &c, &p);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, cases[i].exp_change, c);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, cases[i].exp_pct, p);
  }
}

static void test_null_outputs_safe(void) {
  change_compute(10, 5, NULL, NULL);   // must not crash
  TEST_PASS();
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_change_table);
  RUN_TEST(test_null_outputs_safe);
  return UNITY_END();
}
