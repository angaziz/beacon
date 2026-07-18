#include "ui/rotation.h"
#include <lvgl.h>
#include "ui/lvgl_port.h"
#include "ui/carousel.h"
#include "ui/pal_panel.h"
#include "core/orientation.h"
#include "core/nvs.h"
#include "util/log.h"

static uint8_t s_mode    = ROTATION_AUTO;
static uint8_t s_pending = 0;
static bool    s_has_pending = false;

// True while a transient UI gesture is in flight and a rotation would look like a glitch.
//
// Deliberately NOT lv_anim_count_running(): the mascot's border pulse is an infinite-repeat
// lv_anim (pal_panel.cpp), so that check is permanently true whenever the PAL overlay is open and
// would wedge auto-rotation shut. The gestures that actually matter are the carousel's page snap
// and a finger still on the glass -- both bounded well under a second.
static bool ui_busy(void) {
  if (lvgl_port_touch_down()) return true;
  lv_obj_t* pager = carousel_root();
  return pager && lv_obj_is_scrolling(pager);
}

// Suspend the mascot across the turn: lv_disp_drv_update() invalidates the whole screen, and letting
// the frame timer step mid-repaint animates the mascot through the rotation instead of after it.
static void apply(uint8_t rot) {
  bool pal = pal_panel_is_open();
  if (pal) pal_panel_set_paused(true);
  lvgl_port_set_rotation(rot);
  if (pal) pal_panel_set_paused(false);
}

void rotation_init(void) {
  s_mode = nvs_get_byte("rot", ROTATION_AUTO);
  if (s_mode > ROTATION_AUTO) s_mode = ROTATION_AUTO;      // tolerate a garbage/old key
  uint8_t start = (s_mode == ROTATION_AUTO) ? ORIENT_0 : s_mode;
  orient_reset(start);
  apply(start);
  LOGI("rotation mode=%u start=%u deg", (unsigned)s_mode, (unsigned)start * 90u);
}

void rotation_service(void) {
  if (s_mode != ROTATION_AUTO) return;

  // Latch the newest committed quadrant. If another change lands while we are still deferring, the
  // latch is overwritten rather than queued -- we only ever care about where the device is NOW.
  if (orient_take_change()) { s_pending = orient_stable(); s_has_pending = true; }
  if (!s_has_pending) return;
  if (ui_busy()) return;                       // let the in-flight gesture finish, then turn

  s_has_pending = false;
  if (s_pending != lvgl_port_rotation()) apply(s_pending);
}

uint8_t rotation_mode(void) { return s_mode; }

void rotation_set_mode(uint8_t m) {
  if (m > ROTATION_AUTO) return;
  s_mode = m;
  nvs_set_byte("rot", m);
  s_has_pending = false;
  if (m == ROTATION_AUTO) {
    orient_reset(lvgl_port_rotation());   // keep what is on screen; gravity takes over from here
  } else {
    apply(m);
  }
}
