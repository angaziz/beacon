#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include "util/log.h"
#include "hal/power.h"
#include "hal/display.h"
#include "hal/touch.h"
#include "ui/lvgl_port.h"
#include "core/datastore.h"
#include "config/ticker_table.h"
#include "core/timekeep.h"
#include "core/nvs.h"
#include "core/location.h"
#include "core/net.h"
#include "core/provision.h"
#include "core/fetch_task.h"
#include "core/hub_task.h"
#include "ui/styles.h"
#include "ui/theme.h"
#include "ui/carousel.h"
#include "ui/pair_overlay.h"
#include "ui/dev_seed.h"
#include "ui/idle_glue.h"
#include "ui/capture.h"
#include "hal/imu.h"
#include "core/imu_detect.h"
#include "ui/overlays.h"
#if defined(BEACON_AUDIO_SPIKE)
#include "hal/audio.h"
#endif

static lv_obj_t* setup_step(lv_obj_t* card, const beacon_theme_t* t, const char* txt) {
  lv_obj_t* l = lv_label_create(card);   // default font: reliable full-ASCII (theme fonts are subset)
  lv_obj_set_width(l, 296);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(l, t->ink, 0);
  lv_label_set_text(l, txt);
  return l;
}

// First-boot / re-provision setup card: a clear, step-by-step instruction. The SSID is framed as a
// network (accent pill) so it reads as a Wi-Fi name to join from another device, not a label.
static void show_provision_overlay(void) {
  const beacon_theme_t* t = theme_active();
  lv_obj_t* card = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(card);
  lv_obj_set_width(card, 344);
  lv_obj_set_height(card, LV_SIZE_CONTENT);
  lv_obj_center(card);
  lv_obj_set_style_bg_color(card, t->bg, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, t->line, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 14, 0);
  lv_obj_set_style_pad_all(card, 22, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);   // info only: swipes fall through to the carousel
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(card, 11, 0);

  lv_obj_t* eb = lv_label_create(card);
  lv_obj_set_style_text_color(eb, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(eb, 3, 0);
  lv_label_set_text(eb, "WI-FI SETUP");

  setup_step(card, t, "1  On a phone, tablet, or laptop, open its Wi-Fi settings");
  setup_step(card, t, "2  Join this Wi-Fi network:");

  lv_obj_t* pill = lv_obj_create(card);   // frame the SSID like a network entry
  lv_obj_remove_style_all(pill);
  lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_hor(pill, 16, 0);
  lv_obj_set_style_pad_ver(pill, 8, 0);
  lv_obj_set_style_radius(pill, 8, 0);
  lv_obj_set_style_border_color(pill, t->accent, 0);
  lv_obj_set_style_border_width(pill, 1, 0);
  lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* ssid = lv_label_create(pill);
  lv_obj_set_style_text_color(ssid, t->accent, 0);
  lv_label_set_text(ssid, provision_ap_ssid());

  setup_step(card, t, "3  A setup page will open. Enter the Wi-Fi you want to connect to there.");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  LOGI("boot - core=%s", ESP_ARDUINO_VERSION_STR);
  enableLoopWDT();

  nvs_begin();        // open persisted settings before anything reads them
  location_begin();   // load the cached place/coords/source (issue #54) before fetch/UI read them

  if (!power_begin())   { LOGE("halt: power");   return; }
  delay(120);
  if (!display_begin()) { LOGE("halt: display"); return; }
  display_brightness(nvs_get_brightness(204));   // restore persisted brightness (FR-SET-2); 204 = 80% default
  touch_begin();
  if (!imu_begin()) LOGE("imu: not detected (gestures disabled)");
  // Escape hatch: holding a finger on the screen during boot forces the setup portal even with stored
  // creds (recovery when you're on a new network and can't reach the saved one). ~0.6s sample window.
  bool force_provision = false;
  { int16_t tx, ty; int held = 0;
    for (int i = 0; i < 30; i++) { if (touch_read(&tx, &ty)) held++; delay(20); }
    force_provision = held > 22; }
  if (force_provision) LOGI("boot touch-hold => forcing provisioning portal");
  timekeep_init();   // PCF85063 RTC on the shared Wire bus; NTP starts later (after WiFi up)
#if defined(BEACON_AUDIO_SPIKE)
  // Audio init after I2C bus is up (power_begin) and display is live. Wire shared — no re-init.
  if (!audio_init()) LOGW("audio: init failed — heap spike will log only, no chime");
#endif
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) { LOGE("halt: no PSRAM (LVGL pool needs it)"); return; }
  LOGI("psram total=%u free=%u", (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
       (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  if (!lvgl_port_begin()) { LOGE("halt: lvgl"); return; }

  ticker_table_init();   // seed the runtime ticker table before fetch/UI read it
  datastore_init();   // seeds finance_count + ids (screens read these at build time)
  styles_init();
  carousel_init();
  idle_init();
#if !BEACON_CAPTURE
  if (provision_needed() || force_provision) {   // first boot OR touch-hold recovery: host the setup AP
    provision_begin();
    show_provision_overlay();
  } else {
    net_begin();                   // WiFi STA from NVS creds; starts NTP on GOT_IP (non-blocking)
  }
#endif
  // Capture build stays fully offline: WiFi/NTP/esp async logs would interleave into the binary
  // screenshot stream and corrupt frame alignment. Dev seed supplies the data instead.
#if BEACON_DEV
  dev_seed_init();                 // fake data + state-cycler harness (no network/BLE)
#else
  fetch_task_start();              // Core-0 scheduler: real weather + finance + staleness sweep
  hub_task_start();                // Core-0 hub plane: BLE link -> usage + buddy (P2)
#endif
  LOGI("setup done; swipe to navigate");
}

void loop() {
#if defined(BEACON_AUDIO_SPIKE)
  {
    static uint32_t s_heap_at   = 0;
    static uint32_t s_chime_at  = 0;
    static uint8_t  s_chime_idx = 0;
    uint32_t now = millis();
    if (now - s_heap_at >= 1000u) { s_heap_at = now; audio_spike_log(); }
    if (now - s_chime_at >= 15000u) { s_chime_at = now; audio_play_chime(s_chime_idx++ & 1); }
  }
#endif
  provision_loop();    // no-op unless the setup portal is active
  pair_overlay_service();  // show/hide the BLE passkey card while a hub is bonding (Core-1)
  timekeep_service();  // perform any staged RTC write here (Core-1, serialized with touch on I2C)
  lvgl_port_tick();
#if BEACON_CAPTURE
  capture_service();   // 'C' over serial => sweep every theme x screen to the host
#endif
  idle_service();
  buddy_wake_service();
  uint8_t g = imu_poll();
  if (g & IMU_RAISE) lv_disp_trig_activity(NULL);   // wake from dim/sleep
  if (g & IMU_SHAKE) ui_dismiss_top_overlay();
  delay(5);
}
