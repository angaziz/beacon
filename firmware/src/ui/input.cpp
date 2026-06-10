#include "ui/input.h"
#include <Arduino.h>
#include <math.h>
#include <lvgl.h>
#include "hal/display.h"
#include "hal/imu.h"
#include "core/idle_policy.h"
#include "core/motion_gesture.h"
#include "core/nvs.h"
#include "ui/carousel.h"
#include "ui/brightness_overlay.h"
#include "ui/theme_panel.h"
#include "ui/wifi_panel.h"
#include "util/log.h"

static const uint32_t IDLE_DIM_MS    = 30000;    // dim after 30 s idle
static const uint32_t IDLE_SLEEP_MS  = 300000;   // sleep after 5 min idle (matches Settings "Sleep 5 min")
static const uint8_t  IDLE_DIM_LEVEL = 40;       // ~15% backlight while dimmed (never above the user level)
static const uint32_t IMU_POLL_MS    = 60;       // ~16 Hz accel poll (well under the 125 Hz ODR)

static disp_power_t  s_power = DISP_AWAKE;
static volatile bool s_wake_touch = false;
static motion_det_t  s_motion;
static uint32_t      s_last_imu_ms = 0;
static bool          s_imu_up = false;

static void set_power(disp_power_t p) {
  if (p == s_power) return;
  bool was_asleep = (s_power == DISP_ASLEEP);
  uint8_t restore = nvs_get_brightness(204);
  switch (p) {
    case DISP_AWAKE:
      if (was_asleep) display_set_on(true);
      display_brightness(restore);
      break;
    case DISP_DIM:
      if (was_asleep) display_set_on(true);
      display_brightness(restore < IDLE_DIM_LEVEL ? restore : IDLE_DIM_LEVEL);
      break;
    case DISP_ASLEEP:
      display_set_on(false);
      break;
  }
  s_power = p;
}

// Shake / programmatic back: close the topmost dismissable overlay. pair_overlay is excluded -- it is
// pairing-state-driven and would just re-show. Returns true if something was dismissed.
static bool dismiss_topmost(void) {
  if (brightness_overlay_is_open()) { brightness_overlay_close(); return true; }
  if (theme_panel_is_open())        { theme_panel_close();        return true; }
  if (wifi_panel_is_open())         { wifi_panel_close();         return true; }
  return false;
}

static void longpress_cb(lv_event_t*) { carousel_invoke_context_action(); }

static void gesture_cb(lv_event_t*) {
  if (lv_indev_get_gesture_dir(lv_indev_get_act()) == LV_DIR_BOTTOM && !brightness_overlay_is_open())
    brightness_overlay_open();
}

void input_init(void) {
  s_imu_up = imu_begin();
  motion_init_default(&s_motion);
  // LVGL routes LV_EVENT_GESTURE to the screen (the carousel pages/pager set GESTURE_BUBBLE), and the
  // pages set EVENT_BUBBLE so a page long-press climbs to the screen too -- so both bind here, not the
  // pager (which neither event reaches). See carousel.cpp.
  lv_obj_t* scr = lv_scr_act();
  lv_obj_add_event_cb(scr, longpress_cb, LV_EVENT_LONG_PRESSED, NULL);
  lv_obj_add_event_cb(scr, gesture_cb,   LV_EVENT_GESTURE,      NULL);
  LOGI("input: imu=%s", s_imu_up ? "up" : "absent");
}

bool input_is_asleep(void) { return s_power == DISP_ASLEEP; }
void input_note_wake_touch(void) { s_wake_touch = true; }

void input_service(void) {
  brightness_overlay_service();   // auto-close the quick-brightness card after its idle timeout

  // 1) IMU poll -> motion event (throttled).
  motion_event_t ev = MOTION_NONE;
  uint32_t nowms = millis();
  if (s_imu_up && (nowms - s_last_imu_ms) >= IMU_POLL_MS) {
    s_last_imu_ms = nowms;
    float ax, ay, az;
    if (imu_read_accel(&ax, &ay, &az)) {
      int16_t mg = (int16_t)(sqrtf(ax*ax + ay*ay + az*az) * 1000.0f);
      ev = motion_feed(&s_motion, mg, nowms);
    }
  }

  // 2) Wake: while dimmed/asleep, a touch or any motion restores the panel (FR-PLAT-6/7).
  if (s_power != DISP_AWAKE && (s_wake_touch || ev == MOTION_WAKE || ev == MOTION_SHAKE)) {
    lv_disp_trig_activity(NULL);   // reset inactivity so the policy below resolves to AWAKE
  } else if (s_power == DISP_AWAKE && ev == MOTION_SHAKE) {
    // 3) Shake while awake = dismiss the open overlay (DESIGN.md: no carousel back-stack).
    if (dismiss_topmost()) lv_disp_trig_activity(NULL);
  }
  s_wake_touch = false;

  // 4) Apply the idle power policy from the (possibly just-reset) inactivity timer.
  set_power(idle_power_for(lv_disp_get_inactive_time(NULL), IDLE_DIM_MS, IDLE_SLEEP_MS));
}
