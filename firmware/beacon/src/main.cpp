#include <Arduino.h>
#include "util/log.h"
#include "hal/power.h"
#include "hal/display.h"
#include "hal/touch.h"
#include "ui/lvgl_port.h"
#include "ui/test_screen.h"

void setup() {
  Serial.begin(115200);
  delay(300);
  LOGI("boot — core=%s", ESP_ARDUINO_VERSION_STR);
  enableLoopWDT();   // tech.md §6: watchdog on (arduino-esp32 leaves the loop WDT OFF by default)

  if (!power_begin())     { LOGE("halt: power");   return; }
  delay(120);
  if (!display_begin())   { LOGE("halt: display"); return; }
  touch_begin();
  if (!lvgl_port_begin()) { LOGE("halt: lvgl");    return; }
  test_screen_show();
  LOGI("setup done; boot-to-render %lu ms", (unsigned long)millis());
}

void loop() {
  lvgl_port_tick();
  delay(5);
}
