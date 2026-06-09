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

// A hub ack for an in-flight decision (issue #8): apply the truthful outcome to the buddy prompt's
// confirm state. ok:true clears the prompt; ok:false / err keeps it so the UI can warn "too late".
static void apply_ack(const hub_ack_t& ack) {
  buddy_rec_t b = ds_get_buddy();
  if (!hub_apply_ack(&b, &ack)) {           // stale/mismatched id or nothing pending => ignore
    LOGW("hub: ack id=%s ignored (no matching pending prompt)", ack.id);
    return;
  }
  ds_set_buddy(&b);
  LOGI("hub: ack id=%s ok=%d is_err=%d -> state=%u", ack.id, ack.ok, ack.is_err,
       (unsigned)b.prompt.decision_state);
}

// Inbound frame (loop() context, Core-0). Acks dispatch before status (an ack is not a status frame).
// Status: fill from current records so an absent block keeps its values; stamp last_updated = now so
// staleness ages hub data on the same epoch as P1 (one epoch).
static void on_frame(const char* json, size_t len) {
  hub_ack_t ack;
  if (hub_parse_ack(json, len, &ack)) { apply_ack(ack); return; }

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

bool buddy_decide(bool approve) {
  buddy_rec_t r = ds_get_buddy();
  // Canonical guard (folds in the old per-view present/offline checks + the re-tap guard): only the
  // first decision on a live, present prompt is enqueued.
  if (!r.prompt.present) return false;
  if (r.hdr.state == ST_HUB_OFFLINE || r.hdr.state == ST_RECONNECTING) return false;
  if (r.prompt.decision_state != PROMPT_IDLE_DECISION) return false;     // already decided/awaiting ack
  if (!hub_send_permission(r.prompt.id, approve)) return false;          // keep prompt as-is if not enqueued
  r.prompt.decision_state = PROMPT_PENDING;                              // await the truthful ack; DO NOT clear present
  ds_set_buddy(&r);
  return true;
}

bool buddy_dismiss(void) {
  buddy_rec_t r = ds_get_buddy();
  if (!r.prompt.present || r.prompt.decision_state != PROMPT_TOO_LATE) return false;
  r.prompt.present = false;
  ds_set_buddy(&r);
  return true;
}
