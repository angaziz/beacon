#include <unity.h>
#include "core/motion_gesture.h"

static motion_det_t d;
// peak=200mg, gap=50ms, window=1000ms, 4 peaks => shake, 500ms refractory.
void setUp(void) { motion_config(&d, 200, 50, 1000, 4, 500); }
void tearDown(void) {}

// Steady 1 g (1000 mg) and sub-threshold wiggles never fire.
static void test_rest_is_silent(void) {
  struct { int16_t mg; uint32_t t; } s[] = {
    {1000,0},{1000,60},{1050,120},{980,180},{1100,240},{1000,300},
  };
  for (size_t i = 0; i < sizeof(s)/sizeof(s[0]); i++)
    TEST_ASSERT_EQUAL_INT(MOTION_NONE, motion_feed(&d, s[i].mg, s[i].t));
}

// A single flick's first peak is WAKE (any deliberate motion wakes), never SHAKE.
static void test_single_flick_wakes(void) {
  TEST_ASSERT_EQUAL_INT(MOTION_NONE, motion_feed(&d, 1000, 0));    // no prev yet
  TEST_ASSERT_EQUAL_INT(MOTION_WAKE, motion_feed(&d, 1400, 60));   // jerk 400 => peak #1
}

// Four alternating peaks inside the window => SHAKE on the 4th; the earlier ones are WAKE.
static void test_shake_on_fourth_peak(void) {
  TEST_ASSERT_EQUAL_INT(MOTION_NONE,  motion_feed(&d, 1000, 0));
  TEST_ASSERT_EQUAL_INT(MOTION_WAKE,  motion_feed(&d, 1400, 60));
  TEST_ASSERT_EQUAL_INT(MOTION_WAKE,  motion_feed(&d, 1000, 120));
  TEST_ASSERT_EQUAL_INT(MOTION_WAKE,  motion_feed(&d, 1400, 180));
  TEST_ASSERT_EQUAL_INT(MOTION_SHAKE, motion_feed(&d, 1000, 240));
}

// After a shake, the refractory window swallows everything, then detection resumes.
static void test_refractory_after_shake(void) {
  motion_feed(&d, 1000, 0); motion_feed(&d, 1400, 60); motion_feed(&d, 1000, 120);
  motion_feed(&d, 1400, 180);
  TEST_ASSERT_EQUAL_INT(MOTION_SHAKE, motion_feed(&d, 1000, 240));  // refractory until 740
  TEST_ASSERT_EQUAL_INT(MOTION_NONE,  motion_feed(&d, 1400, 300));  // swallowed
  TEST_ASSERT_EQUAL_INT(MOTION_NONE,  motion_feed(&d, 1000, 700));  // still swallowed
  TEST_ASSERT_EQUAL_INT(MOTION_WAKE,  motion_feed(&d, 1400, 760));  // resumed: peak #1 again
}

// Jerks closer than peak_gap_ms collapse to one peak (one physical jerk != many).
static void test_debounce_gap(void) {
  motion_feed(&d, 1000, 0);
  TEST_ASSERT_EQUAL_INT(MOTION_WAKE, motion_feed(&d, 1400, 60));   // peak
  TEST_ASSERT_EQUAL_INT(MOTION_NONE, motion_feed(&d, 1000, 80));   // 20ms < 50ms gap => ignored
}

// Peaks that fall outside the window are pruned, so four peaks spread too far never make a shake.
static void test_window_expiry_blocks_shake(void) {
  motion_feed(&d, 1000, 0);
  motion_feed(&d, 1400, 60);   // p1
  motion_feed(&d, 1000, 120);  // p2
  motion_feed(&d, 1400, 180);  // p3 (mag stays 1400 through the gap below => no peaks)
  // long quiet stretch, then a 4th peak after p1..p3 have aged out of the 1000ms window
  TEST_ASSERT_EQUAL_INT(MOTION_WAKE, motion_feed(&d, 1000, 1500));  // pruned to 1 peak => WAKE, not SHAKE
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_rest_is_silent);
  RUN_TEST(test_single_flick_wakes);
  RUN_TEST(test_shake_on_fourth_peak);
  RUN_TEST(test_refractory_after_shake);
  RUN_TEST(test_debounce_gap);
  RUN_TEST(test_window_expiry_blocks_shake);
  return UNITY_END();
}
