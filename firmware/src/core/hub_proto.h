#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "core/records.h"   // usage_rec_t, buddy_rec_t, *_LEN capacities
#include "core/screen_state.h"     // data_err_t
#include "config/ticker_table.h"   // ticker_runtime_t, MAX_TICKERS bounds

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

// --- Inbound: hub -> device location block (issue #54) ---
// Parsed from a status frame's optional "loc" block. Coordinates persist for the weather fetcher; the
// name is the CLGeocoder place shown on the time screen; tz is the IANA zone for the clock.
typedef struct { float lat, lon; char tz[40]; char name[48]; } hub_loc_t;

// Parse the optional "loc" block out of a (v:1) status frame, independently of usage/buddy (loc may
// ride the (re)connect full frame or arrive alone in a loc-only frame). Returns true and fills *out
// only when a "loc" object is present; false on invalid JSON, v != 1, or no "loc". Strings truncate
// to their capacities. Kept separate from hub_parse_status so existing callers/tests stay unchanged.
bool hub_parse_loc(const char* json, size_t len, hub_loc_t* out);

// --- Outbound: device -> hub command builders ---
// Write a newline-terminated command frame into buf. Returns bytes written (incl. the '\n', excl. the
// NUL), or 0 on overflow / invalid args. `id` echoes the originating (short) prompt id.
size_t hub_build_permission(char* buf, size_t cap, const char* id, bool approve);

// --- Inbound: hub -> device ack/err (after a command) ---
typedef struct { char id[BUDDY_ID_LEN]; bool ok; bool is_err; } hub_ack_t;
// Parse {"v":1,"ack":"<id>","ok":true} or {"v":1,"err":"<reason>","id":"<id>"}. `id` is the acked /
// rejected prompt id. Returns false if the frame is neither an ack nor an err (or v != 1 / invalid).
bool hub_parse_ack(const char* json, size_t len, hub_ack_t* out);

// Pure state transition for a received ack against the current buddy record (issue #8). Applies only
// when the ack's id matches a PENDING prompt: ok && !is_err => PROMPT_SENT_OK (KEEPS present so the UI
// can hold a brief "sent ok" beat; the device tick clears it after BUDDY_CONFIRM_HOLD_S); otherwise =>
// PROMPT_TOO_LATE + keeps present. A stale/mismatched id leaves *buddy untouched. Returns true if the
// record was changed (so the caller knows to persist + log). Codec stays clock-free. Host-testable.
bool hub_apply_ack(buddy_rec_t* buddy, const hub_ack_t* ack);

// --- Inbound: hub -> device ticker config (chunked snapshot, design §2) ---
// A config frame: {"v":1,"config":{"rev":R,"part":P,"parts":N,"tickers":[{...row...}]}}.
// Wire enum strings map to firmware enums: src {binance,yahoo}, kind {fx,crypto,index,etf},
// basis {prev_close,24h}. Rows concatenated across parts in part order == display order.

// One parsed chunk. rows[]/row_count are this chunk's rows only (the accumulator concatenates).
typedef struct {
  uint32_t        rev;
  int             part;
  int             parts;
  ticker_runtime_t rows[MAX_TICKERS];
  int             row_count;
} config_chunk_t;

// Parse ONE config chunk frame. Returns ERR_NONE on a structurally valid chunk (v==1, a "config"
// object, integer rev/part/parts, a tickers array whose every row maps cleanly to enums and fits the
// length bounds). On failure returns ERR_PARSE and sets *err_out to the static ack-err string that the
// accumulator should surface: "malformed" (bad JSON / missing required field / over-length / empty id /
// too many rows in one chunk), or "bad_source"/"bad_kind"/"bad_basis" for an unknown enum string. On
// success *err_out is left untouched. err_out may be NULL.
data_err_t hub_parse_config_chunk(const char* json, size_t len, config_chunk_t* out, const char** err_out);

// Accumulator across parts. Caller owns the struct (no globals); zero it before the first chunk.
typedef enum { CFG_PENDING, CFG_DONE, CFG_ERR } config_status_t;
typedef struct {
  uint32_t         rev;
  int              parts;
  int              next_part;     // the part index expected next (0-based)
  ticker_runtime_t rows[MAX_TICKERS];
  int              row_count;
  bool             active;        // a partial snapshot is in progress
} config_accum_t;

// Feed one parsed chunk. part 0 (re)starts accumulation for chunk->rev (resets any prior partial).
// Parts must arrive contiguously 0..parts-1; a gap/duplicate/out-of-range part, or a non-part-0 chunk
// whose rev differs from the in-progress rev, => CFG_ERR "bad_chunking". Appending past MAX_TICKERS =>
// CFG_ERR "too_many_tickers". On the last part: finalize => CFG_DONE (rows/row_count hold the full list)
// when count in [1,MAX_TICKERS], else CFG_ERR "empty". On any CFG_ERR the partial is discarded and
// *err_out is set to the static ack-err string. (NVS/apply errors are handled by the caller in A5.)
config_status_t hub_config_accum_step(config_accum_t* acc, const config_chunk_t* chunk, const char** err_out);

// --- Outbound: device -> hub config ack (uses the cmd channel, NOT the ack field) ---
// ok  => {"v":1,"cmd":"config_ack","rev":R,"ok":true,"count":N}\n
// err => {"v":1,"cmd":"config_ack","rev":R,"ok":false,"err":"E"}\n
// Returns bytes written (incl. '\n', excl. NUL), or 0 on overflow / invalid args (cf. hub_build_permission).
size_t hub_build_config_ack(char* buf, size_t cap, uint32_t rev, bool ok, const char* err, int count);

// --- Outbound: device -> hub ticker report (chunked running-table snapshot, issue #105) ---
// Pure, host-testable. The report mirrors the config ROW schema but rides the cmd channel:
// {"v":1,"cmd":"report","what":"tickers","rev":0,"part":P,"parts":N,"tickers":[{...row...}]}.
// rev is always 0 (the device does not persist the hub's rev). Split plan/frame so the caller
// serializes+sends one HUB_FRAME_MAX frame at a time (materializing all chunks would blow the
// hub task's 8 KB stack).

// Greedy whole-row chunk planner. Fills group_start[g] with the first-row index of chunk g and
// returns the chunk count (== parts), 1..MAX_TICKERS. Returns 0 on failure: count < 1 or
// > MAX_TICKERS, a null arg, an unmappable enum, or a single row that alone exceeds the ~900 B budget.
int hub_report_plan(const ticker_runtime_t* rows, int count, int group_start[MAX_TICKERS]);

// Serialize rows[lo..hi) as one newline-terminated cmd:"report" frame (part `part` of `parts`) into
// buf. Returns bytes (incl. '\n', excl. NUL), or 0 on overflow / unmappable enum / bad range.
size_t hub_build_report_frame(const ticker_runtime_t* rows, int lo, int hi,
                              int part, int parts, char* buf, size_t cap);

#ifdef __cplusplus
}
#endif
