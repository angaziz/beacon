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
#include "ui/rotation.h"
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

// Shared card chrome for the two first-boot overlays below.
static lv_obj_t* make_setup_card(const beacon_theme_t* t) {
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
  return card;
}

static lv_obj_t* card_eyebrow(lv_obj_t* card, const beacon_theme_t* t, const char* txt) {
  lv_obj_t* eb = lv_label_create(card);
  lv_obj_set_style_text_color(eb, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(eb, 3, 0);
  lv_label_set_text(eb, txt);
  return eb;
}

// First-boot card when no Wi-Fi is saved. Wi-Fi is OPTIONAL: usage, sessions, permission prompts and
// (since the hub weather frame) weather all arrive over BLE, and the clock runs off the RTC. So the
// first thing to tell a new user is to pair the hub -- not to go set up Wi-Fi they may never need.
// Wi-Fi provisioning now lives in Settings > Wi-Fi, on demand.
static lv_obj_t* s_hub_card = nullptr;

static void show_hub_overlay(void) {
  const beacon_theme_t* t = theme_active();
  lv_obj_t* card = make_setup_card(t);
  s_hub_card = card;

  card_eyebrow(card, t, "CONNECT THE HUB");
  setup_step(card, t, "1  On your Mac, install and open the Beacon Hub app");
  setup_step(card, t, "2  Bluetooth pairing starts automatically -- confirm the passkey shown on the Mac");
  setup_step(card, t, "3  That's it. Usage, sessions and weather arrive over Bluetooth.");
  setup_step(card, t, "Wi-Fi is optional and only adds the markets screen. Add it any time in Settings > Wi-Fi.");
}

// Drop the card once the hub is actually connected -- that IS the success signal it was asking for.
static void hub_card_tick(lv_timer_t* tm) {
  if (!s_hub_card) { lv_timer_del(tm); return; }
  if (!hub_is_connected()) return;
  lv_obj_del(s_hub_card);
  s_hub_card = nullptr;
  lv_timer_del(tm);
}

// Touch-hold recovery only: host the AP and show how to reach it. The SSID is framed as a network
// (accent pill) so it reads as a Wi-Fi name to join from another device, not a label.
static void show_provision_overlay(void) {
  const beacon_theme_t* t = theme_active();
  lv_obj_t* card = make_setup_card(t);

  card_eyebrow(card, t, "WI-FI SETUP");
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
  rotation_init();   // after carousel_init: ui_busy() reads carousel_root(), and apply() repaints it
  idle_init();
#if !BEACON_CAPTURE
  if (force_provision) {           // touch-hold recovery: host the setup AP right away
    provision_begin();
    show_provision_overlay();
  } else {
    // No saved Wi-Fi is NOT a provisioning emergency any more: point the user at the hub instead and
    // leave the setup AP down (an idle AP is pure battery cost on a device that may never need Wi-Fi).
    // net_begin() runs either way so a network added later in Settings > Wi-Fi connects without a reboot.
    if (provision_needed()) {
      show_hub_overlay();
      lv_timer_create(hub_card_tick, 500, NULL);
    }
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
  uint8_t g = imu_poll();          // also feeds core/orientation for rotation_service() below
  if (g & IMU_RAISE) lv_disp_trig_activity(NULL);   // wake from dim/sleep
  if (g & IMU_SHAKE) ui_dismiss_top_overlay();
  rotation_service();
  delay(5);
}
