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
static void test_mod(void) {
  struct { int a, n, expect; } c[] = {
    {0, 6, 0}, {6, 6, 0}, {7, 6, 1}, {-1, 6, 5}, {-7, 6, 5}, {5, 6, 5},
  };
  for (size_t i = 0; i < sizeof(c)/sizeof(c[0]); i++)
    TEST_ASSERT_EQUAL_INT(c[i].expect, carousel_mod(c[i].a, c[i].n));
}
// Recycling map: current pinned at center slot (count/2 = 3 for 6 screens). Adjacent and both
// wrap boundaries must resolve to ordinary +/-1 logical moves.
static void test_logical_at(void) {
  struct { int current, slot, count, expect; } c[] = {
    {0, 3, 6, 0},   // center slot is always the current screen
    {0, 4, 6, 1},   // next
    {0, 2, 6, 5},   // prev: first -> last wrap
    {5, 4, 6, 0},   // next: last -> first wrap
    {5, 3, 6, 5}, {5, 2, 6, 4},
    {2, 4, 6, 3}, {2, 2, 6, 1},
  };
  for (size_t i = 0; i < sizeof(c)/sizeof(c[0]); i++)
    TEST_ASSERT_EQUAL_INT(c[i].expect, carousel_logical_at(c[i].current, c[i].slot, c[i].count));
}
static void test_slot_of(void) {
  struct { int current, logical, count, expect; } c[] = {
    {0, 0, 6, 3}, {0, 1, 6, 4}, {0, 5, 6, 2}, {5, 0, 6, 4}, {2, 3, 6, 4},
  };
  for (size_t i = 0; i < sizeof(c)/sizeof(c[0]); i++)
    TEST_ASSERT_EQUAL_INT(c[i].expect, carousel_slot_of(c[i].current, c[i].logical, c[i].count));
}
// slot_of is the inverse of logical_at for every (current, logical) pair.
static void test_slot_logical_roundtrip(void) {
  for (int current = 0; current < 6; current++)
    for (int logical = 0; logical < 6; logical++)
      TEST_ASSERT_EQUAL_INT(logical,
        carousel_logical_at(current, carousel_slot_of(current, logical, 6), 6));
}
int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_clamp_no_wrap);
  RUN_TEST(test_index_for_x);
  RUN_TEST(test_mod);
  RUN_TEST(test_logical_at);
  RUN_TEST(test_slot_of);
  RUN_TEST(test_slot_logical_roundtrip);
  return UNITY_END();
}
