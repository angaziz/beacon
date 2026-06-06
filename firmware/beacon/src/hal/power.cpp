#define XPOWERS_CHIP_AXP2101
#include "power.h"
#include <Wire.h>
#include <XPowersLib.h>
#include "config/pins.h"
#include "util/log.h"

static XPowersPMU s_pmu;

bool power_begin() {
  Wire.begin(PIN_IIC_SDA, PIN_IIC_SCL);
  bool ok = s_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, PIN_IIC_SDA, PIN_IIC_SCL);
  if (!ok) { LOGE("AXP2101 begin FAIL (check I2C)"); return false; }
  s_pmu.setDC1Voltage(3300);
  s_pmu.setALDO1Voltage(3300);
  s_pmu.setALDO2Voltage(3300);   // DSI_PWR_EN — critical for the 2.16 panel
  s_pmu.setALDO4Voltage(3300);
  s_pmu.enableALDO1();
  s_pmu.enableALDO2();
  s_pmu.enableALDO3();           // display rail
  s_pmu.enableALDO4();
  LOGI("AXP2101 rails ALDO1-4 @3.3V");
  return true;
}
