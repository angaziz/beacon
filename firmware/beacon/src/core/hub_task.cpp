#include "core/hub_task.h"
#include "core/hublink_ble.h"
#include "core/hub_proto.h"
#include "core/datastore.h"
#include "core/timekeep.h"
#include "util/log.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

static HubLinkBle s_link;
static HubLink*   g_link = nullptr;   // null until the task starts (BEACON_DEV leaves it null)
static bool       s_was_connected = false;
static uint32_t   s_min_int_free  = UINT32_MAX;

// Inbound status frame (loop() context, Core-0). Fill from current records so an absent block keeps
// its values; stamp last_updated = now so staleness ages hub data on the same epoch as P1 (one epoch).
static void on_frame(const char* json, size_t len) {
  usage_rec_t u = ds_get_usage();
  buddy_rec_t b = ds_get_buddy();
  bool hu = false, hb = false;
  if (!hub_parse_status(json, len, &u, &hu, &b, &hb)) { LOGW("hub: bad/ignored frame"); return; }
  uint32_t now = (uint32_t)timekeep_now();
  if (hu) { u.hdr.last_updated = now; ds_set_usage(&u); }   // setter forces ST_LIVE
  if (hb) { b.hdr.last_updated = now; ds_set_buddy(&b); }
}

static void hub_task(void*) {
  s_link.onFrame(on_frame);
  if (!s_link.begin()) { LOGE("hub: BLE begin failed; task exiting"); vTaskDelete(nullptr); return; }
  g_link = &s_link;

  int k = 0;
  for (;;) {
    s_link.loop();

    bool c = s_link.isConnected();
    if (s_was_connected && !c) ds_set_hub_offline();   // edge-triggered: flip both hub records once
    s_was_connected = c;

    uint32_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (f < s_min_int_free) s_min_int_free = f;        // running min for the P2 heap re-measure
    if (++k % 500 == 0)                                // ~ every 10 s at 50 Hz
      LOGI("hub: conn=%d int_free=%u min=%u", c, (unsigned)f, (unsigned)s_min_int_free);

    vTaskDelay(pdMS_TO_TICKS(20));   // ~50 Hz: snappy permission round-trip
  }
}

void hub_task_start(void) {
  xTaskCreatePinnedToCore(hub_task, "hub", 8192, nullptr, 1, nullptr, 0);   // Core 0
}

bool hub_send_permission(const char* id, bool approve) {
  if (!g_link) { LOGI("buddy decide (no hub link; local clear) id=%s approve=%d", id, approve); return true; }
  char buf[96];
  size_t n = hub_build_permission(buf, sizeof(buf), id, approve);
  if (!n) return false;
  bool ok = g_link->send(buf, n);
  LOGI("buddy decide -> hub id=%s approve=%d sent=%d", id, approve, ok);
  return ok;
}

bool hub_send_launch(const char* text) {
  if (!g_link) return false;
  char buf[160];
  size_t n = hub_build_launch(buf, sizeof(buf), text);
  if (!n) return false;
  return g_link->send(buf, n);
}
