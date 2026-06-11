#include "core/hub_task.h"
#include "core/hublink_ble.h"
#include "core/hub_proto.h"
#include "core/datastore.h"
#include "core/location.h"
#include "core/timekeep.h"
#include "util/log.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

uint32_t uptime_s(void);   // monotonic uptime (defined in timekeep.cpp; ui/screen.h owns the public decl)

// Cheap byte-level scan: `json` is NOT NUL-terminated (deserializeJson takes len), so use memcmp.
// Gates the full ack parse (issue #65 M4) so status frames don't pay for an ArduinoJson parse twice.
static bool frame_has(const char* j, size_t n, const char* key) {
  size_t k = strlen(key);
  if (n < k) return false;
  for (size_t i = 0; i + k <= n; i++) if (memcmp(j + i, key, k) == 0) return true;
  return false;
}

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
  if (b.prompt.decision_state == PROMPT_SENT_OK) b.prompt.decided_at = uptime_s();  // start confirm-hold
  ds_set_buddy(&b);
  LOGI("hub: ack id=%s ok=%d is_err=%d -> state=%u", ack.id, ack.ok, ack.is_err,
       (unsigned)b.prompt.decision_state);
}

// Inbound frame (loop() context, Core-0). Acks dispatch before status (an ack is not a status frame).
// Status: fill from current records so an absent block keeps its values; stamp last_updated = now so
// staleness ages hub data on the same epoch as P1 (one epoch).
static void on_frame(const char* json, size_t len) {
  if (frame_has(json, len, "\"ack\"") || frame_has(json, len, "\"err\"")) {
    hub_ack_t ack;
    if (hub_parse_ack(json, len, &ack)) { apply_ack(ack); return; }
  }

  // A "loc" block (issue #54) may ride the (re)connect full frame or arrive alone. Parsed independently
  // of usage/buddy; persist via core/location (hub source wins) + apply tz OUTSIDE any location lock.
  hub_loc_t loc;
  if (hub_parse_loc(json, len, &loc)) {
    location_set_from_hub(loc.lat, loc.lon, loc.tz, loc.name);
    if (loc.tz[0] && timekeep_tz_supported(loc.tz)) timekeep_set_tz(loc.tz);
  }

  usage_rec_t u = ds_get_usage();
  buddy_rec_t b = ds_get_buddy();
  buddy_prompt_t prev = b.prompt;                          // snapshot before parse (r1 #4)
  bool hu = false, hb = false;
  if (!hub_parse_status(json, len, &u, &hu, &b, &hb)) { LOGW("hub: bad/ignored frame"); return; }
  uint32_t now  = (uint32_t)timekeep_now();                // wall, for last_updated (staleness/age)
  uint32_t mono = uptime_s();                              // monotonic, for prompt lifecycle stamps
  if (hu) { u.hdr.last_updated = now; ds_set_usage(&u); }  // setter forces ST_LIVE
  if (hb) {
    if (prev.present && prev.decision_state == PROMPT_SENT_OK && !b.prompt.present)
      b.prompt = prev;                                     // protect the in-flight "sent ok" beat from an absent-prompt status (r1 #3)
    else if (b.prompt.present && (!prev.present || strncmp(prev.id, b.prompt.id, BUDDY_ID_LEN) != 0))
      b.prompt.shown_at = mono;                            // genuinely new prompt => start the countdown
    b.hdr.last_updated = now; ds_set_buddy(&b);
  }
}

static void hub_task(void*) {
  s_link.onFrame(on_frame);
  if (!s_link.begin()) { LOGE("hub: BLE begin failed; task exiting"); vTaskDelete(nullptr); return; }
  g_link = &s_link;

  int k = 0;
  for (;;) {
    s_link.loop();
    ds_tick_buddy_prompt(uptime_s());   // prompt expiry + confirm-hold (monotonic; runs every screen)

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
  if (r.hdr.state == ST_HUB_OFFLINE) return false;
  if (r.prompt.decision_state != PROMPT_IDLE_DECISION) return false;     // already decided/awaiting ack
  if (!hub_send_permission(r.prompt.id, approve)) return false;          // keep prompt as-is if not enqueued
  if (!g_link)                                                           // no hub => no ack will ever arrive (dev/seed
    r.prompt.present = false;                                            // path); clear locally as the old per-view flow did
  else
    r.prompt.decision_state = PROMPT_PENDING;                            // real link: await the truthful ack; keep present
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
