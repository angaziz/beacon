#include "core/hub_task.h"
#include "core/hublink_ble.h"
#include "core/hub_proto.h"
#include "core/hub_report.h"
#include "core/datastore.h"
#include "core/location.h"
#include "core/timekeep.h"
#include "config/ticker_table.h"
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
static bool       s_reported = false;   // emitted the once-per-connection ticker report? (issue #105)
static uint32_t   s_min_int_free  = UINT32_MAX;

// Reassembled-across-parts config snapshot (design §2). Single-threaded: on_frame runs only on Core-0.
static config_accum_t s_cfg_accum;

// Send a config_ack back to the hub over the SAME link permission acks use (g_link->send). On the
// native host g_link is always null (no BLE), so this no-ops there.
static void send_config_ack(uint32_t rev, bool ok, const char* err, int count) {
  if (!g_link) return;
  char buf[96];
  size_t n = hub_build_config_ack(buf, sizeof(buf), rev, ok, err, count);
  if (n) g_link->send(buf, n);
  LOGI("hub: config_ack rev=%u ok=%d err=%s count=%d", (unsigned)rev, ok, err ? err : "", count);
}

// Snapshot the device's current ticker table and emit it to the hub as chunked cmd:"report" frames so a
// fresh hub can adopt the list it already holds (issue #105). The chunk serialize/flush/send loop lives in
// hub_emit_report (host-tested, issue #106). Returns true only if EVERY chunk was accepted: a mid-stream
// failure returns false so the caller does NOT latch s_reported and retries the whole report on the next
// inbound frame this connection -- a retry restarts at part 0, which restarts the hub accumulator (the hub
// adopts only on the final part, so a partial delivery never half-adopts). g_link null (native) => true.
static bool send_ticker_report(void) {
  if (!g_link) return true;
  int count = ticker_table_count();
  if (count > MAX_TICKERS) count = MAX_TICKERS;
  ticker_runtime_t rows[MAX_TICKERS];
  for (int i = 0; i < count; i++) {
    if (!ticker_table_get(i, &rows[i])) return false;  // table shrank under us => retry next frame
  }
  int parts = hub_emit_report(g_link, rows, count);
  if (parts < 0) return false;                         // serialize/enqueue failed => retry whole report later
  LOGI("hub: ticker report sent (%d rows, %d parts)", count, parts);
  return true;
}

// A config frame (chunked ticker snapshot, design §3.3). Parse => accumulate => on the last part
// persist+swap the table, reseed the DataStore finance slots, then ack. Fail closed: any error keeps
// the current list and acks ok:false. The scheduler/UI pick up the swap via ticker_table_gen().
static void on_config(const char* json, size_t len) {
  config_chunk_t chunk; memset(&chunk, 0, sizeof(chunk));   // rev defaults to 0 if parse fails before reading it
  const char* err = nullptr;
  if (hub_parse_config_chunk(json, len, &chunk, &err) != ERR_NONE) {
    send_config_ack(chunk.rev, false, err ? err : "malformed", 0);   // parser sets rev best-effort
    return;
  }
  const char* aerr = nullptr;
  switch (hub_config_accum_step(&s_cfg_accum, &chunk, &aerr)) {
    case CFG_PENDING: return;                                          // await more parts
    case CFG_ERR:     send_config_ack(s_cfg_accum.rev, false, aerr, 0); return;
    case CFG_DONE:    break;
  }

  if (!ticker_table_apply(s_cfg_accum.rows, s_cfg_accum.row_count)) {  // NVS write failed => no swap
    send_config_ack(s_cfg_accum.rev, false, "nvs_write_failed", 0);
    return;
  }
  char ids[MAX_TICKERS][FIN_ID_LEN];
  for (int i = 0; i < s_cfg_accum.row_count; i++) {
    strncpy(ids[i], s_cfg_accum.rows[i].id, FIN_ID_LEN - 1);
    ids[i][FIN_ID_LEN - 1] = 0;
  }
  ds_reseed_finance(ids, s_cfg_accum.row_count);                       // new ids + ST_LOADING
  send_config_ack(s_cfg_accum.rev, true, nullptr, s_cfg_accum.row_count);
}

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
  if (!s_reported) {                 // first inbound frame this connection proves the central is listening
    s_reported = send_ticker_report();   // latch only on full success; emit BEFORE dispatch (on_frame has
                                         // early returns for ack/config below). A failed send retries next frame.
  }
  if (frame_has(json, len, "\"ack\"") || frame_has(json, len, "\"err\"")) {
    hub_ack_t ack;
    if (hub_parse_ack(json, len, &ack)) { apply_ack(ack); return; }
  }

  // A config frame (chunked ticker snapshot) carries neither "ack" nor "err"; dispatch it before the
  // loc/status fall-through so it isn't silently swallowed by hub_parse_status.
  if (frame_has(json, len, "\"config\"")) { on_config(json, len); return; }

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
    if (s_was_connected && !c) { ds_set_hub_offline(); s_reported = false; }   // re-report next connection
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
