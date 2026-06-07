#include <Arduino.h>
#include "util/log.h"
#include "hal/power.h"
#include "hal/display.h"
#include "hal/touch.h"
#include "ui/lvgl_port.h"
#include "ui/demo_screen.h"
#include "core/datastore.h"
#include "core/stale.h"

// P0-B on-device DataStore smoke: scripted transitions logged over serial (state-priority proof).
static void datastore_smoke(void) {
  datastore_init();
  weather_rec_t w; memset(&w, 0, sizeof(w));
  w.temp_c = 30.0f; w.hdr.last_updated = 100;
  ds_set_weather(&w);
  LOGI("ds weather=%d (expect LIVE=1)", ds_get_weather().hdr.state);
  ds_tick_staleness(100 + WEATHER_STALE_S);
  LOGI("ds weather=%d after sweep (expect STALE=2)", ds_get_weather().hdr.state);
  ds_set_state_weather(ST_ERROR, ERR_RATE_LIMITED);
  ds_tick_staleness(100 + WEATHER_STALE_S * 10);
  LOGI("ds weather=%d (ERROR not clobbered, expect 4)", ds_get_weather().hdr.state);
  ds_set_hub_offline();
  LOGI("ds usage=%d buddy=%d (expect HUB_OFFLINE=5)",
       ds_get_usage().hdr.state, ds_get_buddy().hdr.state);
  LOGI("ds finance_count=%d", ds_get_finance_count());
}

void setup() {
  Serial.begin(115200);
  delay(300);
  LOGI("boot — core=%s", ESP_ARDUINO_VERSION_STR);
  enableLoopWDT();

  if (!power_begin())     { LOGE("halt: power");   return; }
  delay(120);
  if (!display_begin())   { LOGE("halt: display"); return; }
  touch_begin();
  if (!lvgl_port_begin()) { LOGE("halt: lvgl");    return; }

  datastore_smoke();
  demo_screen_init();
  LOGI("setup done; tap to cycle themes");
}

void loop() {
  lvgl_port_tick();
  delay(5);
}
