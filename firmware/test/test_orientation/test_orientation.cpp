#include <unity.h>
#include "core/orientation.h"

void setUp(void) { orient_reset(ORIENT_0); } void tearDown(void) {}

// Hold one attitude for `ms` at ~25Hz (the rate hal/imu.cpp polls at), starting at t0.
static uint32_t hold(float ax, float ay, uint32_t ms, uint32_t t0) {
  uint32_t t = t0;
  for (; t <= t0 + ms; t += 40) orient_feed(ax, ay, 0.1f, t);
  return t;
}

// The four attitudes in the accel frame, per the measured mounting (see ORIENT_NOTE in
// orientation.cpp). Named so a future axis change is a one-line edit here, not a hunt through asserts.
#define UPRIGHT     0.0f,  1.0f
#define LEFT_DOWN   1.0f,  0.0f
#define UPSIDE_DOWN 0.0f, -1.0f
#define RIGHT_DOWN -1.0f,  0.0f

static void test_upright_stays_zero(void) {
  hold(UPRIGHT, 2000, 0);
  TEST_ASSERT_EQUAL_UINT8(ORIENT_0, orient_stable());
  TEST_ASSERT_FALSE(orient_take_change());
}

static void test_flat_on_desk_holds_current(void) {
  // Face-up: in-plane component is noise well under ORIENT_FLAT_G. Must not drift off ORIENT_0.
  hold(0.05f, -0.03f, 3000, 0);
  TEST_ASSERT_EQUAL_UINT8(ORIENT_0, orient_stable());
  TEST_ASSERT_FALSE(orient_take_change());
}

// The charging case that motivated all this: stood on its left side, the UI must turn 270 so content
// reads upright. Verified on hardware -- this exact attitude rendered correctly.
static void test_left_side_down_is_270(void) {
  hold(LEFT_DOWN, 2000, 0);
  TEST_ASSERT_EQUAL_UINT8(ORIENT_270, orient_stable());
  TEST_ASSERT_TRUE(orient_take_change());
  TEST_ASSERT_FALSE(orient_take_change());   // consume-on-read
}

static void test_brief_tilt_does_not_commit(void) {
  // Turned for less than ORIENT_DWELL_MS, then back upright: no commit (this is the "picked it up
  // and put it down" case that would otherwise flip the UI mid-glance).
  uint32_t t = hold(LEFT_DOWN, 300, 0);
  hold(UPRIGHT, 1000, t);
  TEST_ASSERT_EQUAL_UINT8(ORIENT_0, orient_stable());
  TEST_ASSERT_FALSE(orient_take_change());
}

static void test_diagonal_is_dead_band(void) {
  // 45 deg: neither axis wins by ORIENT_HYST, so nothing is committed however long it is held.
  hold(0.71f, 0.71f, 3000, 0);
  TEST_ASSERT_EQUAL_UINT8(ORIENT_0, orient_stable());
  TEST_ASSERT_FALSE(orient_take_change());
}

static void test_all_four_quadrants(void) {
  uint32_t t = hold(LEFT_DOWN, 1000, 0);
  TEST_ASSERT_EQUAL_UINT8(ORIENT_270, orient_stable());
  t = hold(UPSIDE_DOWN, 1000, t);
  TEST_ASSERT_EQUAL_UINT8(ORIENT_180, orient_stable());
  t = hold(RIGHT_DOWN, 1000, t);
  TEST_ASSERT_EQUAL_UINT8(ORIENT_90, orient_stable());
  hold(UPRIGHT, 1000, t);
  TEST_ASSERT_EQUAL_UINT8(ORIENT_0, orient_stable());
}

static void test_flip_flop_does_not_commit_midway(void) {
  // Alternating between two quadrants faster than the dwell must never commit either.
  uint32_t t = 0;
  for (int i = 0; i < 6; i++) {
    t = hold(LEFT_DOWN, 200, t);
    t = hold(UPRIGHT, 200, t);
  }
  TEST_ASSERT_EQUAL_UINT8(ORIENT_0, orient_stable());
  TEST_ASSERT_FALSE(orient_take_change());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_upright_stays_zero);
  RUN_TEST(test_flat_on_desk_holds_current);
  RUN_TEST(test_left_side_down_is_270);
  RUN_TEST(test_brief_tilt_does_not_commit);
  RUN_TEST(test_diagonal_is_dead_band);
  RUN_TEST(test_all_four_quadrants);
  RUN_TEST(test_flip_flop_does_not_commit_midway);
  return UNITY_END();
}
