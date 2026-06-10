#pragma once
#include <stdbool.h>
// QMI8658 accelerometer over the shared I2C bus (accel only; gyro left off to save power). imu_begin()
// assumes power_begin() already brought Wire up. All callers run on Core-1 (same as touch/RTC), so the
// shared bus stays serialized. Used by ui/input.cpp for flick/shake/raise gestures (FR-PLAT-6).
bool imu_begin();                              // true if the QMI8658 answered on I2C
bool imu_read_accel(float* ax, float* ay, float* az);  // true if a fresh sample was read (g units)
