#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "core/records.h"   // usage_rec_t, buddy_rec_t, *_LEN capacities

// Pure, host-testable codec for the FROZEN tech.md §7.1 hub<->device BLE protocol
// (UTF-8, newline-delimited JSON, every frame carries "v":1). No Arduino/BLE deps: only
// <ArduinoJson.h> + records.h + libc, so [env:native] unit-tests it (mirrors fetch/parse_*).
// The Bluedroid transport (core/hublink_ble) owns sockets/queues; this file owns bytes<->records.

#ifdef __cplusplus
extern "C" {
#endif

// --- Frame reassembly (a frame may span BLE writes; a write may hold a fragment, tech.md §7.1) ---
#define HUB_FRAME_MAX 1024   // longest accepted frame (worst-case status frame ~600 B); longer => dropped

typedef void (*hub_line_cb)(const char* line, size_t len, void* user);  // one whole frame, '\n'/'\r' stripped, NUL-terminated

typedef struct {
  char     buf[HUB_FRAME_MAX];
  size_t   len;
  bool     overflow;   // current frame exceeded HUB_FRAME_MAX => discard until the next '\n'
  uint32_t drops;      // count of dropped oversize frames (observability)
} hub_reassembler_t;

void hub_reassembler_reset(hub_reassembler_t* r);
// Feed raw inbound bytes; invokes cb once per completed frame (empty frames skipped).
void hub_reassembler_feed(hub_reassembler_t* r, const char* data, size_t n, hub_line_cb cb, void* user);

// --- Inbound: hub -> device status frame ---
// Parse ONE reassembled status frame. When present, the "usage"/"buddy" block fills *usage / *buddy
// (value fields only; the caller stamps hdr.last_updated and routes to ds_set_usage/ds_set_buddy).
// *had_usage / *had_buddy report which blocks were present (an absent block leaves its record's
// value fields untouched). Returns false (fills nothing) if the JSON is invalid or "v" != 1.
// Rules (tech.md §7.1/§7.2): a null/absent window pct => -1 (unavailable); strings truncate to
// their *_LEN; an absent buddy.prompt => prompt.present=false (idle).
bool hub_parse_status(const char* json, size_t len,
                      usage_rec_t* usage, bool* had_usage,
                      buddy_rec_t* buddy, bool* had_buddy);

// --- Outbound: device -> hub command builders ---
// Write a newline-terminated command frame into buf. Returns bytes written (incl. the '\n', excl. the
// NUL), or 0 on overflow / invalid args. `id` echoes the originating (short) prompt id.
size_t hub_build_permission(char* buf, size_t cap, const char* id, bool approve);
size_t hub_build_launch(char* buf, size_t cap, const char* text);

// --- Inbound: hub -> device ack/err (after a command) ---
typedef struct { char id[BUDDY_ID_LEN]; bool ok; bool is_err; } hub_ack_t;
// Parse {"v":1,"ack":"<id>","ok":true} or {"v":1,"err":"<reason>","id":"<id>"}. `id` is the acked /
// rejected prompt id. Returns false if the frame is neither an ack nor an err (or v != 1 / invalid).
bool hub_parse_ack(const char* json, size_t len, hub_ack_t* out);

#ifdef __cplusplus
}
#endif
