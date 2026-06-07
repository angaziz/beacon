#include <Arduino.h>
#include <esp_heap_caps.h>
#include "util/log.h"
#include "hal/power.h"
#include "hal/display.h"
#include "hal/touch.h"
#include "ui/lvgl_port.h"
#include "core/datastore.h"
#include "ui/styles.h"
#include "ui/carousel.h"
#include "ui/dev_seed.h"

void setup() {
  Serial.begin(115200);
  delay(300);
  LOGI("boot - core=%s", ESP_ARDUINO_VERSION_STR);
  enableLoopWDT();

  if (!power_begin())   { LOGE("halt: power");   return; }
  delay(120);
  if (!display_begin()) { LOGE("halt: display"); return; }
  touch_begin();
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) { LOGE("halt: no PSRAM (LVGL pool needs it)"); return; }
  LOGI("psram total=%u free=%u", (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
       (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  if (!lvgl_port_begin()) { LOGE("halt: lvgl"); return; }

  datastore_init();   // seeds finance_count + ids (screens read these at build time)
  styles_init();
  carousel_init();
  dev_seed_init();
  LOGI("setup done; swipe to navigate");
}

void loop() {
  lvgl_port_tick();
  delay(5);
}
