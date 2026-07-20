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
#define BUDDY_SID_LEN      8   // "s" + up to 6 digits + NUL
#define BUDDY_LABEL_LEN   29   // 28 chars + NUL (design §4 cap)
#define BUDDY_SESSIONS_MAX 5
#define USAGE_PROVIDERS_MAX 4
#define USAGE_ID_LEN        13   // wire id <=12 ascii chars + NUL
#define USAGE_LABEL_LEN     11   // display label <=10 chars + NUL

enum {                          // wire `state` string => firmware enum
  BST_WORKING = 0,
  BST_WAITING,
  BST_WAITING_QUEUED,
  BST_ATTENTION,
  BST_IDLE,
  BST_QUESTION,
};
typedef struct {
  char     id[BUDDY_SID_LEN];   // opaque hub-minted s<n>, echoed back on tap (Phase 2)
  char     label[BUDDY_LABEL_LEN];
  char     agent[USAGE_ID_LEN]; // owning provider id (wire "agent"); empty when absent
  uint8_t  state;               // BST_*
  uint32_t ts;                  // epoch seconds of last update (sort key, age source)
} buddy_session_t;

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
// `stale` (#108): the windows carry last-known-good held by the hub through a transient failure (e.g.
// Claude oauth 429). Views dim the provider's windows; the shared hdr stays for hub-link state.
typedef struct {
  char id[USAGE_ID_LEN];        // stable lowercase provider id (wire "id"); empty slot => ""
  char label[USAGE_LABEL_LEN];  // display label (wire "label")
  usage_window_t h5, d7;
  bool stale;
} usage_provider_t;
typedef struct {
  record_hdr_t     hdr;        // ST_HUB_OFFLINE when the hub link drops
  uint8_t          count;      // active providers in p[] (0..USAGE_PROVIDERS_MAX)
  usage_provider_t p[USAGE_PROVIDERS_MAX];
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
// open_state: device-local tap-to-open lifecycle (issue #110, Phase 2). NOT on the wire.
enum {
  OPEN_NONE    = 0,            // no in-flight/just-finished open (memset-zero default)
  OPEN_SENDING = 1,            // open command enqueued; awaiting hub ack
  OPEN_OK      = 2,            // hub acked ok:true -> session focused
  OPEN_FAIL    = 3,            // hub acked err -> focus failed
};
#define BUDDY_OPEN_HOLD_S    2u   // how long OPEN_OK/FAIL feedback stays on screen
#define BUDDY_OPEN_TIMEOUT_S 8u   // OPEN_SENDING times out (no ack) after this many seconds
// Prompt lifecycle timeouts (monotonic seconds): a prompt nobody decides expires; an applied decision
// holds its "sent ok" beat briefly before clearing. See ds_tick_buddy_prompt.
#define BUDDY_PROMPT_EXPIRY_S 590u   // align to the hub ~600s hold (CC PermissionRequest max); local fail-safe for a dropped hub link
#define BUDDY_CONFIRM_HOLD_S   2u
typedef struct {
  bool present;                // a tool-permission prompt is pending (absence => idle)
  char id[BUDDY_ID_LEN];       // prompt id (echoed back on decide)
  char tool[BUDDY_TOOL_LEN];   // tool name
  char hint[BUDDY_HINT_LEN];   // command hint
  char agent[USAGE_ID_LEN];    // owning provider id (wire "agent"); empty when absent
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
  buddy_session_t sessions[BUDDY_SESSIONS_MAX];  // newest-first (hub-sorted); arrives in its OWN frame
  uint8_t         session_count;
  // Tap-to-open feedback (device-local; issue #110 Phase 2). NOT on the wire; ds_apply_sessions
  // must NOT clear these -- they survive session list refreshes until timed out by ds_tick_open.
  char     open_id[BUDDY_SID_LEN];  // session id of the in-flight / just-finished open
  uint8_t  open_state;              // OPEN_*
  uint32_t open_at;                 // uptime_s() stamp when the open was sent / acked
} buddy_rec_t;
