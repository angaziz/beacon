# Implementation Plan — Issue #12: Device-side connection/decision state gaps (v2, post-Codex)

## Scope correction
The #8 "lying ack" fix already wired the confirm lifecycle: `hub_apply_ack` => `decision_state`; all 7 buddy views render `PROMPT_PENDING` ("sent - awaiting") and `PROMPT_TOO_LATE` ("too late"); an `err` frame already surfaces as TOO_LATE. `PROMPT_SENT_OK` (records.h:66) exists but is instant-cleared, so its beat is never shown.

User decisions: (1) DELETE ST_RECONNECTING; (2) RELATIVE AGE on HUB OFFLINE; (3) ADD held "sent ok" beat.

## Architecture (addresses all Codex findings, round 1 + round 2)
Prompt lifecycle (expiry + confirm-hold) lives in a NEW state-preserving, atomic datastore tick `ds_tick_buddy_prompt(now)` — a sibling of `ds_tick_staleness` (datastore.cpp:108). It mutates `prompt` only, never `hdr.state`, so it cannot erase `ST_HUB_OFFLINE` (r1 #5), and does read-modify-write under the datastore lock (r1 #6, atomic).

**Tick home (r2 #1):** call it from the `hub_task` 50Hz loop (hub_task.cpp:50-63) for the real device — that loop never blocks (vTaskDelay 20ms), unlike `fetch_task` whose `net_service()` can block up to 6s. For dev (BEACON_DEV uses `dev_seed_init` => `stale_task`, NOT hub_task/fetch_task per main.cpp:111-115), call it in dev `stale_task` (dev_seed.cpp:43). NOT in fetch_task. Either way it runs regardless of active screen (r1 #2).

**Monotonic time (r2 #3):** `now_s()`/`timekeep_now()` are both wall-clock `time(nullptr)` (timekeep.cpp:110) which JUMPS on NTP/RTC sync — a prompt stamped pre-sync would expire instantly. Prompt lifecycle must use monotonic uptime. Add `uint32_t uptime_s(void)` (declared in `ui/screen.h` beside now_s; defined in `timekeep.cpp` as `return millis()/1000;` — timekeep.cpp is Arduino-only, excluded from the native test filter). Stamp `shown_at`/`decided_at` with `uptime_s()` and tick with `uptime_s()`. Keep `last_updated = timekeep_now()` (wall) for age display/staleness — that is the existing project model and 5m granularity tolerates jumps. So shown_at/decided_at (monotonic, lifecycle) live in a different epoch from last_updated (wall, display); they are only ever compared against each other. Document this.

Two timestamps (r1 #1): `shown_at` (prompt arrival => expiry countdown) and `decided_at` (ack => confirm-hold). One field cannot serve both.

## Criterion 1 — Delete ST_RECONNECTING (pure deletion; never set, confirmed by Codex)
- `core/screen_state.h:11` — remove enum member + comment.
- `ui/state_view.h:38` — remove `case ST_RECONNECTING`; `:53` remove `|| s == ST_RECONNECTING` from `sv_dim`.
- 7 buddy views: `== ST_HUB_OFFLINE || == ST_RECONNECTING` => `== ST_HUB_OFFLINE`:
  buddy_calm.cpp:146, buddy_hud.cpp:157, buddy_editorial.cpp:54, buddy_oscilloscope.cpp:112, buddy_led.cpp:144, buddy_blueprint.cpp:20, buddy_analog.cpp:22.
- `core/hub_task.cpp:85` — drop `|| r.hdr.state == ST_RECONNECTING`.
- DESIGN.md:130 — one-line note: reconnecting is a Mac-menu-bar concern (peripheral cannot observe it).

## Criterion 2 — Relative last-synced age on HUB OFFLINE (centralized, 1 line)
- `ui/state_view.h:37` `ST_HUB_OFFLINE` case: append age via `age_str(..., record_age_s(h, now))`, mirroring `ST_STALE` (state_view.h:35). Result "HUB OFFLINE 5m". `sv_status` already takes `now`; all views call it => no per-view chip edits.
- Verify usage-view chip buffers >= ~16 chars; widen if any undersized.

## New struct fields + constants
- `core/records.h` buddy_prompt_t: add `uint32_t shown_at;` and `uint32_t decided_at;` (device-local monotonic-uptime stamps, not serialized — update the struct comment).
- `core/records.h` (NOT stale.h — r2 #4: state_view.h includes records.h but not stale.h): add near the PROMPT_ enum `#define BUDDY_PROMPT_EXPIRY_S 25u` (match Mac fail-closed window, audit §3.4) and `#define BUDDY_CONFIRM_HOLD_S 2u`, with a "prompt lifecycle timeouts (monotonic seconds)" comment.

## Criterion 4 — Expiry/countdown
- `core/datastore.{h,cpp}`: add `ds_tick_buddy_prompt(uint32_t now)`:
  ```
  ds_lock_take(s_lock);
  buddy_prompt_t* p = &s_buddy.prompt;
  if (p->present) {
    if (p->decision_state == PROMPT_SENT_OK) {
      if (now - p->decided_at >= BUDDY_CONFIRM_HOLD_S) p->present = false;   // beat shown; clear
    } else if (p->decision_state == PROMPT_IDLE_DECISION) {
      if (now - p->shown_at >= BUDDY_PROMPT_EXPIRY_S) p->decision_state = PROMPT_TOO_LATE;  // expired
    }
  }
  ds_lock_give(s_lock);
  ```
- Call `ds_tick_buddy_prompt(uptime_s())` in the `hub_task` loop (after `s_link.loop()`, hub_task.cpp:51) and in dev `stale_task` (dev_seed.cpp:43, beside ds_tick_staleness).
- Stamp `shown_at` in `hub_task.cpp on_frame` for a genuinely-new prompt (r1 #4 — snapshot prev before parse). Note two clocks: `now` (wall) for last_updated, `mono` (uptime) for shown_at:
  ```
  buddy_prompt_t prev = b.prompt;                 // before hub_parse_status
  ...
  uint32_t now = (uint32_t)timekeep_now();        // wall, for last_updated
  uint32_t mono = uptime_s();                     // monotonic, for prompt lifecycle
  if (hb) {
    if (prev.present && prev.decision_state == PROMPT_SENT_OK && !b.prompt.present)
      b.prompt = prev;                            // protect the in-flight beat from an absent-prompt status (r1 #3)
    else if (b.prompt.present && (!prev.present || strncmp(prev.id, b.prompt.id, BUDDY_ID_LEN) != 0))
      b.prompt.shown_at = mono;                   // new prompt => start countdown (monotonic)
    b.hdr.last_updated = now; ds_set_buddy(&b);
  }
  ```
  (`shown_at`/`decided_at` are untouched by `hub_parse_status`, so they carry over for a same-id prompt.)
- Central helper `buddy_prompt_secs_left(const buddy_rec_t* b, uint32_t now)` in `state_view.h` — elapsed-first to avoid uint32 underflow (r2 #2): `uint32_t e = now - b->prompt.shown_at; return e >= BUDDY_PROMPT_EXPIRY_S ? 0 : BUDDY_PROMPT_EXPIRY_S - e;`. Only meaningful while `PROMPT_IDLE_DECISION`. Views pass `uptime_s()` as `now`.
- 7 views, `default` (idle) prompt case: append secs to the eyebrow in each view's idiom (e.g. "approve? 25s" / "PERMISSION 25s"). snprintf of the returned int only; logic centralized.
- Expiry reuses PROMPT_TOO_LATE => existing dismiss affordance; no new enum, no new view case.

## Criterion 3 — Held "sent ok" beat
- `core/hub_proto.cpp:139-141` `hub_apply_ack` ok path: set `PROMPT_SENT_OK`, KEEP `present=true` (remove the instant `present=false`). Codec stays clock-free.
- `core/hub_proto.h:54` — update the `hub_apply_ack` contract comment (ok no longer clears present; the device tick clears after the hold) (r2 #5).
- `core/hub_task.cpp apply_ack`: after `hub_apply_ack` succeeds, if `b.prompt.decision_state == PROMPT_SENT_OK` stamp `b.prompt.decided_at = uptime_s()` (monotonic).
- `ds_tick_buddy_prompt` clears it after `BUDDY_CONFIRM_HOLD_S` (above).
- 7 views: add `case PROMPT_SENT_OK` rendering ASCII "sent ok" (accent/up color), actions dim. No Unicode checkmark (font glyph not guaranteed).
- BEACON_DEV: g_link==null => acks never arrive, so SENT_OK never fires in dev (acceptable). dev countdown/expiry DO run via stale_task.

## Tests (update + add; table-driven)
- `firmware/test/test_hub_proto/test_main.cpp`: update the ok-ack assertion (now keeps `present=true` + `PROMPT_SENT_OK`); err/too-late cases unchanged.
- `firmware/test/test_datastore/test_main.cpp`: add table-driven cases for `ds_tick_buddy_prompt` — (a) IDLE past expiry => TOO_LATE; (b) IDLE before expiry => unchanged; (c) SENT_OK past hold => present=false; (d) SENT_OK before hold => unchanged; (e) hdr.state==ST_HUB_OFFLINE preserved across the tick.
- `firmware/test/test_state_view/test_main.cpp`: update HUB OFFLINE expectation to include the age suffix.

## dev_seed
- `dev_seed.cpp` seeded prompt: set `b.prompt.shown_at = uptime_s()` so the dev countdown is sane (the seeded prompt will demo expiry after 25s — expected behavior).
- `dev_seed.cpp` stale_task: add `ds_tick_buddy_prompt(uptime_s())` beside `ds_tick_staleness(now_s())` at line 43.

## New function uptime_s()
- `ui/screen.h`: declare `uint32_t uptime_s(void);` beside `now_s`.
- `core/timekeep.cpp`: define `uint32_t uptime_s(void) { return (uint32_t)(millis() / 1000); }` (Arduino-only TU, excluded from native test filter).

## Build/verify
- Host tests: `cd firmware && pio test -e native` (or the host env). Device build: `pio run -e beacon`. Flash for manual test: `pio run -e beacon -t upload`.

## Accepted residual (documented)
`on_frame`/`buddy_decide`/`apply_ack` keep the existing snapshot-then-`ds_set_buddy` pattern; `ds_tick_buddy_prompt` is atomic. A tick firing between an on_frame snapshot and its set can be undone for one ~50Hz cycle, then re-applied next tick (self-correcting, converges). This matches the codebase's existing concurrency model; no new locking introduced.
