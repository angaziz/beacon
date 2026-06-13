#include "ui/capture.h"
#if BEACON_CAPTURE

#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "config/layout.h"
#include "ui/theme.h"
#include "ui/theme_catalog.h"
#include "ui/carousel.h"
#include "core/nvs.h"
#include "util/log.h"

// Full-frame RGB565 buffer (466x466 = ~424KB), PSRAM only. Allocated lazily on the first sweep so
// env:capture pays nothing until you actually press 'C'.
static uint16_t* s_frame = nullptr;
static uint8_t*  s_rle = nullptr;        // RLE16 output (worst case 4 bytes/px); keeps the stream tiny so the host never falls behind
static bool      s_armed = false;
volatile bool    g_log_muted = false;   // gates [BEACON] LOGx (log.h) off mid-sweep so no log byte tears a frame

void capture_blit(const lv_area_t* a, const lv_color_t* px) {
  if (!s_armed || !s_frame) return;
  int32_t x1 = a->x1 < 0 ? 0 : a->x1, y1 = a->y1 < 0 ? 0 : a->y1;
  int32_t x2 = a->x2 >= SCREEN_W ? SCREEN_W - 1 : a->x2;
  int32_t y2 = a->y2 >= SCREEN_H ? SCREEN_H - 1 : a->y2;
  int32_t stride = a->x2 - a->x1 + 1;   // source row pitch is the un-clamped strip width
  const uint16_t* src = (const uint16_t*)px;
  for (int32_t y = y1; y <= y2; y++) {
    const uint16_t* srow = src + (size_t)(y - a->y1) * stride + (x1 - a->x1);
    uint16_t* drow = s_frame + (size_t)y * SCREEN_W + x1;
    for (int32_t x = x1; x <= x2; x++) *drow++ = *srow++;
  }
}

// RLE16: emit [uint16 count][uint16 color] little-endian runs. AMOLED screens are mostly pure black,
// so a 424KB frame collapses to a few KB -- small enough that the host always keeps up (no drops).
static size_t rle_encode(const uint16_t* px, size_t n) {
  size_t o = 0;
  for (size_t i = 0; i < n; ) {
    uint16_t v = px[i];
    size_t run = 1;
    while (i + run < n && px[i + run] == v && run < 0xFFFF) run++;
    s_rle[o++] = run & 0xFF;  s_rle[o++] = run >> 8;
    s_rle[o++] = v & 0xFF;    s_rle[o++] = v >> 8;
    i += run;
  }
  return o;   // worst case 4*n bytes, within the 4-bytes/px buffer
}

// Force a synchronous full repaint of the active screen into s_frame, RLE-compress it, ship it. The
// strips land via flush_cb -> capture_blit (and still reach the panel, so you can watch on the glass).
static void grab_and_stream(const char* theme_id, const char* screen_id) {
  memset(s_frame, 0, (size_t)SCREEN_W * SCREEN_H * sizeof(uint16_t));   // untouched pixels => black (AMOLED bg)
  s_armed = true;
  lv_obj_invalidate(lv_scr_act());   // full-screen object: invalid area covers the whole display (all layers composite)
  lv_refr_now(NULL);                 // blocks until every strip has flushed
  s_armed = false;

  const size_t clen = rle_encode(s_frame, (size_t)SCREEN_W * SCREEN_H);
  Serial.printf("\nFRAME %s %s %d %d %u\n", theme_id, screen_id, (int)SCREEN_W, (int)SCREEN_H, (unsigned)clen);
  Serial.flush();
  for (size_t off = 0; off < clen; off += 4096) {
    size_t n = (clen - off) < 4096 ? (clen - off) : 4096;
    Serial.write(s_rle + off, n);
    Serial.flush();
  }
  Serial.print("\nENDFRAME\n");
  Serial.flush();
}

static void run_sweep(void) {
  const size_t npx = (size_t)SCREEN_W * SCREEN_H;
  if (!s_frame) s_frame = (uint16_t*)heap_caps_malloc(npx * 2, MALLOC_CAP_SPIRAM);
  if (!s_rle)   s_rle   = (uint8_t*)heap_caps_malloc(npx * 4, MALLOC_CAP_SPIRAM);   // worst-case RLE size
  if (!s_frame || !s_rle) { Serial.print("\nCAPTURE ERR: buffer alloc failed\n"); return; }
  const int screens = carousel_count();
  const uint8_t theme0 = nvs_get_theme(DEFAULT_THEME_INDEX);   // restore user's selection after the sweep
  const int     screen0 = carousel_current();
  esp_log_level_set("*", ESP_LOG_NONE);   // silence ESP/ARDUHAL async logs
  g_log_muted = true;                      // silence our own [BEACON] LOGx (incl. Core-0 dev_seed)
  disableLoopWDT();                        // the sweep monopolizes loop() for ~20s; don't let the WDT reset us
  Serial.setTxTimeoutMs(5000);             // CDC drops bytes on TX timeout under the 15MB blast; make writes block instead
  Serial.printf("\nBEACONCAP START %d\n", THEME_COUNT * screens);

  for (int t = 0; t < THEME_COUNT; t++) {
    theme_set((uint8_t)t);                      // rebuilds all pages for this theme (also persists to NVS)
    for (int s = 0; s < screens; s++) {
      carousel_goto(s);
      for (int k = 0; k < 12; k++) {            // let gauges/animations settle (~200ms)
        lv_timer_handler();
        delay(16);
      }
      grab_and_stream(THEME_CATALOG[t].id, carousel_screen_id(s));
    }
  }
  theme_set(theme0);                            // leave the device exactly as the user had it
  carousel_goto(screen0);
  Serial.print("\nBEACONCAP DONE\n");
  Serial.flush();
  Serial.setTxTimeoutMs(100);   // restore a short timeout for normal logging
  enableLoopWDT();
  g_log_muted = false;
  esp_log_level_set("*", ESP_LOG_INFO);
}

void capture_service(void) {
  bool go = false;
  while (Serial.available()) if (Serial.read() == 'C') go = true;   // coalesce: one sweep per batch, not per byte
  if (go) {
    LOGI("capture: starting %dx%d sweep", THEME_COUNT, carousel_count());
    run_sweep();
  }
}

#endif
