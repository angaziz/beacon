#pragma once
#include <stdint.h>

// Pure IMU gesture detection (FR-PLAT-6). Fed accelerometer samples (g, plus a ms timestamp);
// emits a bitmask of recognized gestures. No I2C/Arduino -- host-tested. Thresholds are starting
// values; tune on hardware later.
#ifdef __cplusplus
extern "C" {
#endif

enum { IMU_NONE = 0, IMU_RAISE = 1 << 0, IMU_SHAKE = 1 << 1 };

void    imu_detect_reset(void);
void    imu_detect_feed(float ax, float ay, float az, uint32_t t_ms);
uint8_t imu_detect_poll(void);   // returns + clears pending events since last poll

#ifdef __cplusplus
}
#endif
