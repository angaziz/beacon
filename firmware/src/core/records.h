#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "core/screen_state.h"

// String field rule (frozen): every char[] is a fixed-capacity, NUL-terminated buffer;
// writers MUST truncate to fit (never overflow). Capacities are named so consumers can size buffers.
#define FIN_ID_LEN      16
#define BUDDY_ID_LEN    24
#define BUDDY_TOOL_LEN  24
#define BUDDY_HINT_LEN  80
#define BUDDY_ENTRY_LEN 40
#define BUDDY_ENTRIES    3

typedef struct {
  uint32_t       last_updated;  // epoch seconds of last successful update; 0 = never
  screen_state_t state;
  data_err_t     err;           // cause when state == ST_ERROR; else ERR_NONE
} record_hdr_t;

// Age in seconds since last successful update; UINT32_MAX if never updated.
static inline uint32_t record_age_s(const record_hdr_t* h, uint32_t now) {
  return h->last_updated ? (now - h->last_updated) : UINT32_MAX;
}

// --- Weather (FR-HOME, device-plane) ---
typedef struct {
  record_hdr_t hdr;
  float    temp_c;
  float    humidity_pct;
  uint16_t wmo_code;            // condition; label/icon via WMO_MAP (location.h)
} weather_rec_t;

// --- Finance (FR-FIN, device-plane) — array; each slot independently stateful ---
typedef struct {
  record_hdr_t hdr;            // per-instrument state/age (one may be stale while others live)
  char    id[FIN_ID_LEN];      // stable key, matches ticker_cfg_t.id
  double  value;
  double  change;              // signed absolute change
  double  change_pct;          // signed percent
} finance_rec_t;

// --- AI usage (FR-USAGE, hub-plane) — mirrors tech.md §7.1/§7.2 BLE JSON ---
typedef struct {
  int16_t  pct;                // 0..100; -1 = null/unavailable (JSON null)
  uint32_t reset;              // epoch seconds; 0 = unknown
} usage_window_t;
typedef struct { usage_window_t h5, d7; } usage_provider_t;
typedef struct {
  record_hdr_t     hdr;        // ST_HUB_OFFLINE when the hub link drops
  usage_provider_t claude, codex;
} usage_rec_t;

// --- Coding buddy (FR-BUDDY, hub-plane) ---
// decision_state tracks the local confirm lifecycle of a sent decision so the UI stops lying about
// outcomes (issue #8): a decision is enqueued (PENDING) and only cleared on a truthful hub ack
// (SENT_OK) or surfaced as TOO_LATE when the hub says it did not apply. Device-local only -- NOT on
// the wire (hub_proto serializes id/decision and parses fields individually, never the raw struct).
enum {
  PROMPT_IDLE_DECISION = 0,    // no decision sent yet (memset-zero default)
  PROMPT_PENDING       = 1,    // decision enqueued; awaiting the hub ack
  PROMPT_SENT_OK       = 2,    // hub acked ok:true -> decision applied
  PROMPT_TOO_LATE      = 3,    // hub acked ok:false / err -> decision did not apply (late/superseded)
};
// Prompt lifecycle timeouts (monotonic seconds): a prompt nobody decides expires; an applied decision
// holds its "sent ok" beat briefly before clearing. See ds_tick_buddy_prompt.
#define BUDDY_PROMPT_EXPIRY_S 590u   // align to the hub ~600s hold (CC PermissionRequest max); local fail-safe for a dropped hub link
#define BUDDY_CONFIRM_HOLD_S   2u
typedef struct {
  bool present;                // a tool-permission prompt is pending (absence => idle)
  char id[BUDDY_ID_LEN];       // prompt id (echoed back on decide)
  char tool[BUDDY_TOOL_LEN];   // tool name
  char hint[BUDDY_HINT_LEN];   // command hint
  uint8_t queue_len;           // total pending prompts incl. this front one (1 = lone); from qlen, NOT a local stamp
  uint8_t decision_state;      // device-local confirm lifecycle (PROMPT_*); NOT serialized
  // Device-local monotonic-uptime stamps (uptime_s()), NOT serialized; live in a different epoch from
  // hdr.last_updated (wall clock) and are only ever compared against each other (each-other deltas).
  uint32_t shown_at;           // uptime when this prompt arrived => expiry countdown
  uint32_t decided_at;         // uptime when the hub acked ok => confirm-hold window
} buddy_prompt_t;
typedef struct {
  record_hdr_t  hdr;           // ST_HUB_OFFLINE when the hub link drops
  uint8_t       running, waiting;
  uint32_t      tokens;
  uint8_t       context_pct;
  char          entries[BUDDY_ENTRIES][BUDDY_ENTRY_LEN]; // recent activity (newest first)
  uint8_t       entry_count;
  buddy_prompt_t prompt;
} buddy_rec_t;
