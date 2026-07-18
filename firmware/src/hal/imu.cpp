#include "hal/imu.h"
#include "core/imu_detect.h"
#include "core/orientation.h"
#include <Arduino.h>
#include <Wire.h>
#include <SensorQMI8658.hpp>

static SensorQMI8658 s_qmi;
static bool          s_ok = false;
static uint32_t      s_last_ms = 0;

#define IMU_POLL_MS 40   // ~25 Hz; enough for raise/shake, light on the shared bus

bool imu_begin(void) {
  // QMI8658 default I2C address QMI8658_L_SLAVE_ADDRESS (0x6B). Reuse the existing Wire (already
  // begun by power/touch); do NOT call Wire.begin() again with different pins.
  s_ok = s_qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS);
  if (!s_ok) return false;
  // Accel-only at 4G / 250Hz. Detector only needs accel; leaving the gyro off keeps the chip in
  // async accel mode so getDataReady() polls STATUS0 (no sync/locking handshake needed).
  s_qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_250Hz);
  s_qmi.enableAccelerometer();
  imu_detect_reset();
  return true;
}

uint8_t imu_poll(void) {
  if (!s_ok) return IMU_NONE;
  uint32_t now = millis();
  if (now - s_last_ms < IMU_POLL_MS) return IMU_NONE;
  s_last_ms = now;
  if (!s_qmi.getDataReady()) return IMU_NONE;
  IMUdata acc;
  if (!s_qmi.getAccelerometer(acc.x, acc.y, acc.z)) return IMU_NONE;
  imu_detect_feed(acc.x, acc.y, acc.z, now);   // SensorLib reports accel in g
  orient_feed(acc.x, acc.y, acc.z, now);       // same samples drive screen-orientation detection
  return imu_detect_poll();
}
