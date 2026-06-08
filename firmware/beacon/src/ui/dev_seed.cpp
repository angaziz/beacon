#include "ui/dev_seed.h"
#if BEACON_DEV
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include "core/datastore.h"
#include "core/stale.h"
#include "config/tickers.h"
#include "ui/carousel.h"
#include "ui/screen.h"
#include "util/log.h"

static void seed(void) {
  uint32_t now = now_s();
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
  u.claude.h5 = {24, now + 7680}; u.claude.d7 = {24, now + 400000};
  u.codex.h5  = {1,  now + 16860}; u.codex.d7 = {29, now + 400000};
  u.hdr.last_updated = now; ds_set_usage(&u);
  buddy_rec_t b; memset(&b, 0, sizeof(b)); b.running = 2; b.waiting = 1; b.tokens = 184502; b.context_pct = 42;
  b.prompt.present = true; strncpy(b.prompt.id, "A1B2", BUDDY_ID_LEN-1); strncpy(b.prompt.tool, "Bash", BUDDY_TOOL_LEN-1);
  strncpy(b.prompt.hint, "rm -rf /tmp/build-cache", BUDDY_HINT_LEN-1); b.hdr.last_updated = now; ds_set_buddy(&b);
  nowplaying_rec_t np; memset(&np, 0, sizeof(np)); np.has_device = true; np.playing = true;
  strncpy(np.title, "Midnight City", NP_TITLE_LEN-1); strncpy(np.artist, "M83", NP_ARTIST_LEN-1);
  strncpy(np.device, "Studio", NP_DEVICE_LEN-1); np.progress_ms = 60000; np.duration_ms = 240000; np.hdr.last_updated = now;
  ds_set_nowplaying(&np);
}

// Core-0 staleness ticker (DataStore sweeps are Core-0; P1's fetch task replaces this) + heap gate log.
static void stale_task(void*) {
  int k = 0;
  for (;;) {
    ds_tick_staleness(now_s());
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
    case 4: if (s_phase == 0) seed(); else ds_set_state_nowplaying(dev[s_phase], ERR_NO_ROUTE); break;
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
#else
void dev_seed_init(void) {}
#endif
