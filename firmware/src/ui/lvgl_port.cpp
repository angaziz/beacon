#include "lvgl_port.h"
#include <lvgl.h>
#include <esp_heap_caps.h>
#include "hal/display.h"
#include "hal/touch.h"
#include "config/layout.h"
#include "util/log.h"
#include "ui/idle_glue.h"

static const uint32_t BUF_LINES  = 47;                       // ~1/10 of 466 (tech.md §6)
static const size_t   BUF_PX     = (size_t)SCREEN_W * BUF_LINES;
static const size_t   BUF_BYTES  = BUF_PX * sizeof(lv_color_t);
static const size_t   HEAP_FLOOR = 60u * 1024u;              // tech.md §8

static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t* s_buf1 = nullptr;
static lv_color_t* s_buf2 = nullptr;

static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* a, lv_color_t* px) {
  int32_t w = a->x2 - a->x1 + 1;
  int32_t h = a->y2 - a->y1 + 1;
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

static void indev_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  int16_t x, y;
  if (touch_read(&x, &y)) {
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
  s_buf2 = (lv_color_t*)heap_caps_malloc(BUF_BYTES, caps);
  uint32_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
#else
  s_buf1 = (lv_color_t*)heap_caps_malloc(BUF_BYTES, caps);
  s_buf2 = (lv_color_t*)heap_caps_malloc(BUF_BYTES, caps);
  uint32_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (!s_buf1 || !s_buf2 || free_int < HEAP_FLOOR) {       // fall back to PSRAM
    if (s_buf1) heap_caps_free(s_buf1);
    if (s_buf2) heap_caps_free(s_buf2);
    caps = MALLOC_CAP_SPIRAM; region = "PSRAM";
    s_buf1 = (lv_color_t*)heap_caps_malloc(BUF_BYTES, caps);
    s_buf2 = (lv_color_t*)heap_caps_malloc(BUF_BYTES, caps);
  }
#endif
  if (!s_buf1 || !s_buf2) { LOGE("lvgl buffer alloc FAIL"); return false; }
  lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, BUF_PX);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_W; disp_drv.ver_res = SCREEN_H;
  disp_drv.flush_cb = flush_cb; disp_drv.draw_buf = &s_draw_buf;
  disp_drv.rounder_cb = rounder_cb;   // even-align flush window (CO5300 requirement)
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = indev_read_cb;
  lv_indev_drv_register(&indev_drv);

  free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  LOGI("lvgl buffers in %s (2x %u B); free internal heap=%u floor=%u",
       region, (unsigned)BUF_BYTES, (unsigned)free_int, (unsigned)HEAP_FLOOR);
  if (free_int < HEAP_FLOOR) LOGW("below 60KB internal floor (Chunk-A baseline; remeasure at P2)");
  return true;
}

void lvgl_port_tick() { lv_timer_handler(); }
