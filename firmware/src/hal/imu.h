#pragma once
#include <stdbool.h>
#include <stdint.h>

// QMI8658 6-axis IMU over the shared Wire bus. imu_begin() assumes power_begin() ran (Wire up).
// imu_poll() reads the accelerometer, feeds core/imu_detect, and returns its event bitmask
// (IMU_RAISE / IMU_SHAKE / IMU_NONE). Call from loop() (Core-1, serialized with touch/RTC on I2C).
#ifdef __cplusplus
extern "C" {
#endif

bool    imu_begin(void);   // true if the QMI8658 answered on I2C
uint8_t imu_poll(void);    // sample + detect; IMU_NONE if no device or no gesture

#ifdef __cplusplus
}
#endif
