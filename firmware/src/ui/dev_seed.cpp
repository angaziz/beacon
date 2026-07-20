#include "ui/dev_seed.h"
#if BEACON_DEV
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include "core/datastore.h"
#include "core/location.h"
#include "core/stale.h"
#include "config/tickers.h"
#include "ui/carousel.h"
#include "ui/screen.h"
#include "util/log.h"

// Capture-time buddy state selector (build flag only; does not affect runtime behavior).
// 0 = session list (default), 1 = Approve/Deny prompt card, 2 = question takeover.
#ifndef BEACON_CAP_BUDDY
#define BEACON_CAP_BUDDY 0
#endif

static void seed(void) {
  uint32_t now = now_s();
#if BEACON_CAPTURE
  location_set_place_capture("San Francisco, CA");   // mask the device's real cached place in screenshots (RAM only)
#endif
  weather_rec_t w; memset(&w, 0, sizeof(w)); w.temp_c = 31.8f; w.humidity_pct = 57; w.wmo_code = 2; w.hdr.last_updated = now;
  ds_set_weather(&w);
  const double vals[] = {18026, 62392, 6012, 19540, 52.18, 5594, 16800, 2400, 145, 7600};
  const double chg[]  = {0.12, 2.14, 0.41, 0.63, -0.88, -0.34, 0.05, 1.2, -0.4, 0.9};
  int n = ds_get_finance_count();
  for (int i = 0; i < n; i++) {
    finance_rec_t f; memset(&f, 0, sizeof(f));
    f.value = vals[i % 10]; f.change_pct = chg[i % 10]; f.hdr.last_updated = now;
    ds_set_finance(i, &f);
  }
  usage_rec_t u; memset(&u, 0, sizeof(u));
  u.count = 2;
  strncpy(u.p[0].id, "claude", USAGE_ID_LEN-1); strncpy(u.p[0].label, "CLAUDE", USAGE_LABEL_LEN-1);
  u.p[0].h5 = {72, now + 7680}; u.p[0].d7 = {45, now + 400000};
  strncpy(u.p[1].id, "codex", USAGE_ID_LEN-1); strncpy(u.p[1].label, "CODEX", USAGE_LABEL_LEN-1);
  u.p[1].h5 = {58, now + 16860}; u.p[1].d7 = {33, now + 400000};
  u.hdr.last_updated = now; ds_set_usage(&u);
  buddy_rec_t b; memset(&b, 0, sizeof(b)); b.running = 2; b.waiting = 1; b.tokens = 184502; b.context_pct = 42;
  // No prompt seeded: capture shows the session list (the new design), not the permission screen.
#if BEACON_CAP_BUDDY == 1
  // Prompt state: Approve/Deny card rendered over the session list.
  b.prompt.present = true;
  strncpy(b.prompt.tool, "Bash", BUDDY_TOOL_LEN-1);
  strncpy(b.prompt.hint, "[beacon] rm -rf /tmp/build-cache", BUDDY_HINT_LEN-1);
  b.prompt.queue_len = 2;
#endif
  b.hdr.last_updated = now; ds_set_buddy(&b);
  // Seed 4 representative sessions (newest-first) so the session list renders across all themes.
  buddy_session_t sess[4];
  memset(sess, 0, sizeof(sess));
  strncpy(sess[0].id, "s3", BUDDY_SID_LEN-1); strncpy(sess[0].label, "beacon \xc2\xb7 feat/usage", BUDDY_LABEL_LEN-1);
#if BEACON_CAP_BUDDY == 2
  // Question state: first session shows the "tap to answer on Mac" takeover card.
  sess[0].state = BST_QUESTION;
#else
  sess[0].state = BST_ATTENTION;
#endif
  sess[0].ts = now - 30;
  strncpy(sess[1].id, "s1", BUDDY_SID_LEN-1); strncpy(sess[1].label, "hub \xc2\xb7 main",      BUDDY_LABEL_LEN-1); sess[1].state = BST_WORKING;         sess[1].ts = now - 120;
  strncpy(sess[2].id, "s7", BUDDY_SID_LEN-1); strncpy(sess[2].label, "dotfiles \xc2\xb7 sync", BUDDY_LABEL_LEN-1); sess[2].state = BST_WAITING_QUEUED;  sess[2].ts = now - 300;
  strncpy(sess[3].id, "s2", BUDDY_SID_LEN-1); strncpy(sess[3].label, "notes \xc2\xb7 main",    BUDDY_LABEL_LEN-1); sess[3].state = BST_IDLE;            sess[3].ts = now - 900;
  ds_apply_sessions(sess, 4, now);
}

// Core-0 staleness ticker (DataStore sweeps are Core-0; P1's fetch task replaces this) + heap gate log.
static void stale_task(void*) {
  int k = 0;
  for (;;) {
    ds_tick_staleness(now_s());
    ds_tick_buddy_prompt(uptime_s());
    if (++k % 5 == 0)
      LOGI("heap: int_free=%u int_dma_blk=%u psram_free=%u psram_blk=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// Long-press cycles the visible screen's record through its plane's reachable states.
static int s_phase = 0;
static void longpress_cb(lv_event_t*) {
  int scr = carousel_current(); s_phase = (s_phase + 1) % 4;
  screen_state_t dev[] = {ST_LIVE, ST_STALE, ST_OFFLINE, ST_ERROR};
  switch (scr) {
    case 0: if (s_phase == 0) seed(); else ds_set_state_weather(dev[s_phase], ERR_RATE_LIMITED); break;
    case 1: if (s_phase == 0) seed(); else ds_set_state_finance(0, dev[s_phase], ERR_TIMEOUT); break;
    case 2: case 3: if (s_phase % 2) ds_set_hub_offline(); else seed(); break;  // hub-plane: LIVE<->HUB_OFFLINE
    default: break;
  }
  LOGI("dev: screen=%d phase=%d", scr, s_phase);
}

void dev_seed_init(void) {
  seed();
  xTaskCreatePinnedToCore(stale_task, "stale", 3072, nullptr, 1, nullptr, 0);   // Core 0
  lv_obj_add_event_cb(carousel_root(), longpress_cb, LV_EVENT_LONG_PRESSED, NULL);
}
#if BEACON_CAPTURE
void dev_seed_reseed(void) { seed(); }   // re-stamp records LIVE before a screenshot sweep
#endif
#else
void dev_seed_init(void) {}
#endif
