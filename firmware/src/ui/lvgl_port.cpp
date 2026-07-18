#include "lvgl_port.h"
#include <lvgl.h>
#include <esp_heap_caps.h>
#include "hal/display.h"
#include "hal/touch.h"
#include "config/layout.h"
#include "util/log.h"
#include "ui/idle_glue.h"
#include "ui/capture.h"

static const uint32_t BUF_LINES  = 47;                       // ~1/10 of 466 (tech.md §6)
static const size_t   BUF_PX     = (size_t)SCREEN_W * BUF_LINES;
static const size_t   BUF_BYTES  = BUF_PX * sizeof(lv_color_t);
static const size_t   HEAP_FLOOR = 60u * 1024u;              // tech.md §8

static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t* s_buf1 = nullptr;
static lv_disp_drv_t s_disp_drv;          // file scope: rotation is switched at runtime via drv_update
static lv_disp_t*    s_disp = nullptr;
static uint8_t       s_rot  = LV_DISP_ROT_NONE;
static bool          s_touch_down = false;
// Single draw buffer (#65 M1): flush_cb is synchronous (display_draw_bitmap blocks, then
// lv_disp_flush_ready inline), so LVGL never renders into a second buffer while the first flushes --
// buffer B was dead weight. One buffer frees a full BUF_BYTES (~43.8KB) with zero behavior change.

static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* a, lv_color_t* px) {
  int32_t w = a->x2 - a->x1 + 1;
  int32_t h = a->y2 - a->y1 + 1;
#if BEACON_CAPTURE
  capture_blit(a, px);   // mirror the strip into the screenshot frame (no-op unless a sweep is armed)
#endif
  display_draw_bitmap(a->x1, a->y1, w, h, (uint16_t*)px);
  lv_disp_flush_ready(drv);
}

// CO5300 needs even-aligned window bounds; odd partial-flush coords corrupt pixels.
// Snap x1/y1 down to even and x2/y2 up to odd so width/height stay even (per Waveshare).
static void rounder_cb(lv_disp_drv_t* drv, lv_area_t* a) {
  if (a->x1 & 1) a->x1--;
  if (!(a->x2 & 1)) a->x2++;
  if (a->y1 & 1) a->y1--;
  if (!(a->y2 & 1)) a->y2++;
}

// NOTE: do NOT un-rotate touch coordinates here. LVGL already maps physical -> logical for pointer
// devices in indev_pointer_proc() (lv_indev.c) whenever disp_drv.rotated is set, so this callback
// must hand it RAW panel coordinates. Doing it here as well applies the transform twice, and for
// 90/270 the axis swap then cancels itself -- taps land 180 degrees off and, more visibly, carousel
// swipes keep following the unrotated axis. Cost a debug cycle once; leaving this note so it does not
// cost another.
static void indev_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  int16_t x, y;
  bool down = touch_read(&x, &y);
  s_touch_down = down;   // physical contact, even when the press is swallowed as a wake below
  if (down) {
    // Record whether this press is waking the device BEFORE triggering activity, so the flag is
    // set on every PRESSED (fresh each contact; stale flags can't linger across gestures).
    idle_note_press(idle_is_inactive());
    if (idle_is_inactive()) {                // dimmed or asleep => wake only; don't activate what's under the finger
      lv_disp_trig_activity(NULL);           // LVGL counts only PRESSED as activity; force the reset
      data->state = LV_INDEV_STATE_RELEASED;
      return;
    }
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x; data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

bool lvgl_port_begin() {
  lv_init();

  uint32_t caps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
  const char* region = "internal-SRAM";
#ifdef BEACON_LVGL_PSRAM
  // P2 heap-floor escape valve (tech.md §8 / P2 spec §7): force LVGL draw buffers into PSRAM so the
  // scarce internal SRAM survives an active bonded BLE link + cert TLS. The boot-time auto-fallback
  // below cannot react to that later load (BLE/WiFi start after this), so this is a build-time choice.
  caps = MALLOC_CAP_SPIRAM; region = "PSRAM (forced: BEACON_LVGL_PSRAM)";
  s_buf1 = (lv_color_t*)heap_caps_malloc(BUF_BYTES, caps);
  uint32_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
#else
  s_buf1 = (lv_color_t*)heap_caps_malloc(BUF_BYTES, caps);
  uint32_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (!s_buf1 || free_int < HEAP_FLOOR) {                  // fall back to PSRAM
    if (s_buf1) heap_caps_free(s_buf1);
    caps = MALLOC_CAP_SPIRAM; region = "PSRAM";
    s_buf1 = (lv_color_t*)heap_caps_malloc(BUF_BYTES, caps);
  }
#endif
  if (!s_buf1) { LOGE("lvgl buffer alloc FAIL"); return false; }
  lv_disp_draw_buf_init(&s_draw_buf, s_buf1, nullptr, BUF_PX);

  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res = SCREEN_W; s_disp_drv.ver_res = SCREEN_H;
  s_disp_drv.flush_cb = flush_cb; s_disp_drv.draw_buf = &s_draw_buf;
  s_disp_drv.rounder_cb = rounder_cb;   // even-align flush window (CO5300 requirement)
  s_disp = lv_disp_drv_register(&s_disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = indev_read_cb;
  // LVGL's default scroll_limit (10px) is too tight for this panel: every page sits inside the
  // horizontally-scrollable carousel (carousel.cpp), so any tap with >10px of ordinary finger
  // jitter gets captured by lv_indev_scroll's find_scroll_obj() as the start of a page-swipe
  // instead of delivering LV_EVENT_CLICKED to whatever was tapped (e.g. pal_panel.cpp's mascot,
  // or buddy screen's approve/deny) -- the click is silently dropped, not just delayed. Widen the
  // tap-vs-swipe threshold; still tiny relative to the 466px panel, so deliberate swipes are unaffected.
  indev_drv.scroll_limit = 20;
  lv_indev_drv_register(&indev_drv);

  free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  LOGI("lvgl buffer in %s (%u B); free internal heap=%u floor=%u",
       region, (unsigned)BUF_BYTES, (unsigned)free_int, (unsigned)HEAP_FLOOR);
  if (free_int < HEAP_FLOOR) LOGW("below 60KB internal floor (Chunk-A baseline; remeasure at P2)");
  return true;
}

void lvgl_port_tick() { lv_timer_handler(); }

// Switching rotation re-registers the driver; lv_disp_drv_update() re-runs layout and invalidates the
// whole screen, so the next tick repaints everything in the new orientation. Cheap enough to call
// from the loop (it is gated to actual changes by ui/rotation.cpp).
//
// EVEN-WINDOW INVARIANT (why LV_DISP_ROT_MAX_BUF is sized the way it is in lv_conf.h): the CO5300
// corrupts partial redraws whose window coordinates are odd, which is what rounder_cb guarantees --
// but rounder_cb runs on the UNROTATED area, before LVGL's draw_buf_rotate() re-splits it. That
// splitter emits chunks whose physical width is the chunk's row count, and with a rotation buffer
// too small to take an area in one pass the leftover chunk can land odd. Sizing the buffer to hold
// a whole draw-buffer's worth of pixels (>= 2 * BUF_PX bytes) makes max_row >= area_h for EVERY
// possible area shape, so each area rotates in one chunk (or, for tall areas, an area_w-high square
// plus an even remainder) and rounder_cb's evenness survives to the panel. Do not shrink it.
void lvgl_port_set_rotation(uint8_t rot) {
  rot &= 3;
  if (!s_disp || rot == s_rot) return;
  s_rot = rot;
  s_disp_drv.rotated   = rot;
  s_disp_drv.sw_rotate = (rot != LV_DISP_ROT_NONE) ? 1 : 0;
  lv_disp_drv_update(s_disp, &s_disp_drv);
  LOGI("rotation -> %u deg", (unsigned)rot * 90u);
}

uint8_t lvgl_port_rotation(void) { return s_rot; }

bool lvgl_port_touch_down(void) { return s_touch_down; }
