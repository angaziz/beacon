#include <unity.h>
#include "core/idle.h"

void setUp(void) {} void tearDown(void) {}

// idle_eval maps inactivity to a phase. ms == 0 for either threshold means "never".
static void test_eval(void) {
  const uint32_t D = 60000;   // dim after 1 min
  const uint32_t S = 300000;  // sleep after 5 min
  struct { uint32_t inact, dim, slp; idle_phase_t expect; } c[] = {
    {0,        D, S, IDLE_ACTIVE},
    {59999,    D, S, IDLE_ACTIVE},
    {60000,    D, S, IDLE_DIM},
    {299999,   D, S, IDLE_DIM},
    {300000,   D, S, IDLE_SLEEP},
    {999999,   D, S, IDLE_SLEEP},
    {999999,   0, S, IDLE_SLEEP},
    {120000,   0, S, IDLE_ACTIVE},
    {999999,   D, 0, IDLE_DIM},
    {999999,   0, 0, IDLE_ACTIVE},
    {400000,   S, D, IDLE_SLEEP},
  };
  for (size_t i = 0; i < sizeof(c)/sizeof(c[0]); i++)
    TEST_ASSERT_EQUAL_INT(c[i].expect, idle_eval(c[i].inact, c[i].dim, c[i].slp));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_eval);
  return UNITY_END();
}
