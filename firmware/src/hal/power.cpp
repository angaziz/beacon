#define XPOWERS_CHIP_AXP2101
#include "power.h"
#include <Arduino.h>
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
  s_pmu.enableBattDetection();      // fuel gauge for battery percent
  s_pmu.enableBattVoltageMeasure();
  LOGI("AXP2101 rails ALDO1-4 @3.3V");
  return true;
}

#define POWER_CACHE_MS 10000u
static uint32_t s_batt_at  = 0;   // millis() of last PMU read; 0 forces the first read
static int      s_batt_pct = -1;
static bool     s_batt_chg = false;

static void power_refresh(void) {
  uint32_t now = millis();
  if (s_batt_at != 0 && (uint32_t)(now - s_batt_at) < POWER_CACHE_MS) return;
  s_batt_at = now;
  int p = s_pmu.isBatteryConnect() ? s_pmu.getBatteryPercent() : -1;
  s_batt_pct = (p < 0) ? -1 : (p > 100 ? 100 : p);
  s_batt_chg = s_pmu.isCharging();
}

int  power_battery_pct() { power_refresh(); return s_batt_pct; }
bool power_charging()    { power_refresh(); return s_batt_chg; }
