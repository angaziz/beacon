#include "imu.h"
#include <Wire.h>
#include "SensorQMI8658.hpp"
#include "config/pins.h"
#include "util/log.h"

static SensorQMI8658 s_imu;
static bool s_up = false;

bool imu_begin() {
  // Wire is already begun by power_begin(); this attaches the driver to the existing bus (idempotent
  // pin re-apply on ESP32), same pattern as touch_begin().
  if (!s_imu.begin(Wire, ADDR_IMU, PIN_IIC_SDA, PIN_IIC_SCL)) {
    LOGW("QMI8658 not found at 0x%02X (try 0x6A)", ADDR_IMU);
    return false;
  }
  // Accel only: 4 G is plenty of headroom for flicks/shakes, 125 Hz is well above our ~16 Hz poll and
  // keeps the part in a low-power mode. Gyro stays disabled.
  s_imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_125Hz,
                            SensorQMI8658::LPF_MODE_0);
  s_imu.enableAccelerometer();
  s_up = true;
  LOGI("QMI8658 up (chip 0x%02X)", s_imu.getChipID());
  return true;
}

bool imu_read_accel(float* ax, float* ay, float* az) {
  if (!s_up || !s_imu.getDataReady()) return false;
  return s_imu.getAccelerometer(*ax, *ay, *az);
}
