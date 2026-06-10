#include <unity.h>
#include "core/imu_detect.h"

void setUp(void) { imu_detect_reset(); } void tearDown(void) {}

static void feed(float x, float y, float z, uint32_t t_ms) { imu_detect_feed(x, y, z, t_ms); }

static void test_resting_no_event(void) {
  uint32_t t = 0;
  for (int i = 0; i < 10; i++, t += 50) feed(0.0f, 0.0f, 1.0f, t);
  TEST_ASSERT_EQUAL_INT(IMU_NONE, imu_detect_poll());
}

static void test_shake_fires(void) {
  // x=3.0 => mag=sqrt(9+0+1)=sqrt(10)~3.16, deviation~2.16 >= SHAKE_G(1.8). 2.5 was too low.
  uint32_t t = 0;
  for (int i = 0; i < 6; i++, t += 40) feed(i % 2 ? 3.0f : -3.0f, 0.0f, 1.0f, t);
  TEST_ASSERT_TRUE(imu_detect_poll() & IMU_SHAKE);
}

static void test_raise_fires(void) {
  uint32_t t = 0;
  feed(0.0f, 0.0f, 1.0f, t); t += 60;
  feed(0.0f, 0.7f, 0.2f, t);
  TEST_ASSERT_TRUE(imu_detect_poll() & IMU_RAISE);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_resting_no_event);
  RUN_TEST(test_shake_fires);
  RUN_TEST(test_raise_fires);
  return UNITY_END();
}
