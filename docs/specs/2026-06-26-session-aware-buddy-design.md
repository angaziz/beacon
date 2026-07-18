# Session-Aware Coding Buddy — Design

- **Date:** 2026-06-26
- **Status:** Approved design, pending implementation plan
- **Scope:** Firmware (`firmware/`) + Hub (`hub/`) + BLE contract (`hub/CONTRACT.md`)
- **Reviews:** Two independent Codex passes (findings folded in; see Appendix A)

## 1. Problem & Goal

Today the Coding Buddy speaks only when Claude Code needs an **approval**: the hub
chimes, the device shows a single Approve/Deny prompt. Nothing surfaces the rest of a
session's life — when a session finishes its turn and is waiting on you, or which of
several parallel sessions needs attention.

**Primary scenario:** at the desk, heads-down in another window, running 2-3 Claude Code
sessions in parallel. The user wants *ambient peripheral awareness* of which session
needs them, and a one-tap way to jump to that session's terminal/editor.

**Goal:** evolve the buddy from a *permission gate* into a *glanceable dashboard of every
session's state* — silent when work flows, distinct sounds when a specific session needs
the user, and (later phases) one tap to focus it.

### Non-goals

- No transcript reading or content inspection on the hub (privacy: hub stays out of
  conversation content).
- No fabricated states. `error` is dropped for v1 — there is no clean hook signal for
  "session crashed" (if Claude Code crashes, hooks may not fire at all).
- No device-side audio in v1 (heap-gated; Phase 3).

## 2. State Model

Deliberately minimal and **fully hook-sourceable** — every state derives from a signal
already arriving at the hub. No transcript parsing.

| State | Source | Sound | Wire | Meaning |
|---|---|---|---|---|
| `working` | statusline POST or tool/Pre hook for the session | silent | yes | actively running |
| `waiting` | **held** `PermissionRequest` that owns the live prompt | `beacon-prompt.wav` (existing) | yes | needs an approve/deny decision, actionable now |
| `waiting_queued` | held `PermissionRequest` behind the front one | silent | yes | blocked on a decision, not yet actionable |
| `attention` | `Stop` hook | `beacon-attention.wav` (new) | yes | finished its turn — your turn (question or done; not differentiated) |
| `idle` | no activity for `SESSION_IDLE_TTL` | silent | yes | stopped a while ago, untouched |
| `ended` | `SessionEnd` hook | silent | **no** | removed from registry — absent from the next frame |

**`ended` is a registry lifecycle event, not a wire state.** On `SessionEnd` the hub deletes
the session (matching today's `ClaudeCodeBridge.swift:471-477` removal); it simply disappears
from the next `sessions` frame. It is listed above only to complete the lifecycle.

> **Post-spec addition (issue #110 follow-up):** a `question` state was added after this spec.
> It is driven by the CC `Notification` hook (session waiting on the user's input), sits in the
> precedence below `waiting`/`waiting_queued` and above `attention`, and the device surfaces it as a
> "tap to answer on Mac" takeover (permission prompt > question > list). This intentionally walks back
> the "do not differentiate question vs done" decision below — `Notification` gave a clean
> needs-input signal that `Stop` (which fires every turn) did not. The token/context figures were
> also dropped from the buddy header (kept on the AI Usage screen). See `hub/CONTRACT.md` §A,
> `docs/prd.md` FR-BUDDY-1/8.

**State precedence (resolves overlapping signals).** A session can have several signals live
at once (statusline still ticking while a prompt is held). The hub picks the highest:
`waiting`/`waiting_queued` (a held prompt) **>** `attention` (last lifecycle event was `Stop`,
no newer activity) **>** `working` (any statusline/tool activity within the activity window)
**>** `idle` (no activity for `SESSION_IDLE_TTL`). A new statusline tick or tool hook clears
`attention` back to `working` (Claude resumed).

**Why `attention` does not differentiate "asking a question" vs "done cleanly":** in the
multitasking scenario the user action is identical — go look at that session. The `Stop`
hook fires when Claude finishes its turn; that is the signal. Differentiating would
require transcript reading (flaky heuristics + privacy surface) for no behavioral gain.

**`waiting` vs `waiting_queued` (Codex #5):** the permission queue is a single global FIFO;
only the **front** prompt is device-actionable. The session owning the front prompt is
`waiting`; sessions with prompts queued behind it are `waiting_queued` (rendered dimmer,
no actionable affordance) so the device never implies you can act on a prompt that isn't
the live one.

## 3. Architecture

Heavy lifting on the hub; the device stays minimal (optimal memory use, stable contract).

```
Claude Code hooks ──HTTP──► Hub session registry ──BLE status frame──► Device session list
  PermissionRequest          s<id> → {session_id, cwd, branch,          {id,label,state,ts}
  Stop, SessionStart,         host_app, tab/tty, pid, state,             capped, sorted
  SessionEnd, statusline      created_at, updated_at}                          │
                                      ▲                                        │
                                      │      {v:1,cmd:"open",id:"s3"}  (Ph.2)  │
                              Hub focus resolver ◄──────BLE command───────────┘
                              (tiered, best-effort)
```

- **Hub session registry** (new): keyed by Claude Code `session_id`. Mints a short opaque
  `s<n>` id from a **monotonic counter**, distinct namespace from the existing `p<n>` prompt
  ids. **Reuse/rollover rule:** the counter only increments and is **never reused** for a live
  session; a given `session_id` keeps its `s<n>` for its whole life. To honor the frozen 6-char
  id cap (`s` + ≤5 digits), the counter **wraps modulo 100000**; collisions are impossible in
  practice because reaching 100k sessions in one hub process would require the first to be long
  reaped (`removeTTL` = 600 s). The device treats `s<n>` as an opaque string. Holds full context. Drives sound + frame emission. Reuses the
  existing TTL reaper + tombstone GC; adds a **hard max-size cap**.
- **Device**: holds only `{id, label, state, ts}` per row in a fixed-size
  `buddy_session_t[5]` — no UUIDs, no context. Renders + (Phase 2) taps.

## 4. BLE Contract (additive, stays `v:1`)

Additive change — no version bump. See `hub/CONTRACT.md` §A for the frozen status/buddy frame.

**`sessions` is its OWN frame — NOT a field inside `buddy`.** The existing `StatusFrame`
(`Protocol.swift:78`) already bundles `usage` + `buddy` + `loc` into a single newline-delimited
frame, and the (re)connect "full frame" emits all three together (~600 B today). Embedding a
5-element `sessions` array inside `buddy` would push that combined frame past
`HUB_FRAME_MAX` = 1024 B — and **frames over 1024 B are silently dropped**
(`hub_proto.h:18`). A standalone frame sidesteps the shared budget and decouples session
cadence from the usage/loc heartbeat (same way `loc` already gets its own frame).

**Hub → device, new standalone frame:**

```json
{"v":1,"sessions":[
  {"id":"s3","label":"beacon · fix/109","state":"attention","ts":1719400000},
  {"id":"s1","label":"api · main","state":"working","ts":1719399860}
]}
```

**Frozen caps (this frame's worst case asserted < `HUB_FRAME_MAX` = 1024 B; reassembler
spans BLE notifications — confirmed `hub_proto.cpp` reassembles to `\n`):**

| Field | Cap |
|---|---|
| `sessions` length | ≤ 5 |
| `id` | ≤ 6 chars (`s` + counter) |
| `label` | ≤ 28 chars (folder · branch; branch dropped when redundant) |
| `state` | enum string (`working`/`waiting`/`waiting_queued`/`attention`/`idle`) |
| `ts` | epoch seconds (last update) |

Worst case ≈ 5 × ~85 B ≈ 425 B + envelope ≪ 1024 B. A unit test encodes the max-size
`sessions` frame and asserts < 1024. `ended` is **not** a wire state (§2) — an ended session
is simply absent from the next frame.

**`buddy` frame unchanged → clean migration.** The `buddy` frame keeps emitting `entries`
exactly as today, so **un-updated firmware keeps its activity list**. New firmware renders the
`sessions` frame for the `claude` screen and ignores `entries`; old firmware ignores the
unknown `sessions` frame. No combined-frame size pressure, no version bump, no "emit both in
one frame" coupling. The hub may stop emitting `entries` in a later release once all devices
are updated. CONTRACT.md gains the `sessions` frame spec in §A.

**Device → hub (Phase 2):** new `open` command added to the `DeviceCommand` enum, mirroring
`permission`'s ack/err semantics (see `hub/CONTRACT.md` §B):

**Device → hub (Phase 2):** new `open` command added to the `DeviceCommand` enum, mirroring
`permission`'s ack/err semantics (see `hub/CONTRACT.md` §B):

```json
{"v":1,"cmd":"open","id":"s3"}              // device → hub
{"v":1,"ack":"s3","ok":true}                // focus attempted (best-effort tier succeeded)
{"v":1,"ack":"s3","ok":false}               // could not focus (e.g. permission denied / app gone)
{"v":1,"err":"unknown_session","id":"s3"}   // id the hub never minted / already reaped
```

## 5. Device Session-List Screen (`claude` screen, all 7 themes)

Replaces the chronological event log on the `claude` screen (between usage and settings).
Surface = **7 buddy view files**: `buddy_{analog, blueprint, calm, editorial, hud, led,
oscilloscope}.cpp`. All updated atomically.

- **Up to 4 visible rows** (Codex #1). Safe content height = 466 − 2×40 = **386 px**; the
  min tap target is ~64 px (DESIGN.md). 6 rows do not fit; 4 rows + header do. The registry
  and frame carry up to 5; the device displays the 4 most-recently-updated (sorted by `ts`
  desc). DESIGN.md gains an explicit note on the session-row height.
- **Row** (follows the existing per-view list-row **pattern** — label + value + caret,
  width-capped, ellipsized; note it is a local helper per `*_<theme>.cpp`, not a shared API):
  primary folder label, dimmed branch (hidden when redundant), a **state cue** (theme-token
  color + glyph: amber=waiting, dim-amber=waiting_queued, blue/green=attention, active=working,
  dim=idle), relative age (`2m`).
- **Empty / single-session states.** 0 sessions → an idle empty state ("no active sessions"),
  reusing each theme's existing idle treatment (today's empty-`entries` rendering). 1 session →
  a single centered row. The list grows downward to the 4-row cap; only overflow truncates.
- **Relative age & clock skew (Codex #6).** `ts` is a hub epoch; the device renders age by
  subtracting from its **NTP/RTC-synced** clock (the same source the home-screen clock uses).
  If the device clock is not yet synced at boot, it **suppresses the age** (shows none) rather
  than render a garbage delta. Small hub/device skew (seconds) is acceptable for a relative
  label.
- **Phase 2 — tap a row →** sends `{cmd:"open", id}`; row shows "opening…" then ok/err
  feedback (mirrors the permission `PROMPT_SENT_OK` / `PROMPT_TOO_LATE` pattern). The
  feedback is **pinned by session id** until ack/err/timeout and is **immune to frame
  re-sort** (Codex #6) — a later frame re-sorting the list must not yank the tapped row.
- **Memory:** fixed-size `buddy_session_t[5]` in `buddy_rec_t` (records.h), parsed in
  `hub_proto.cpp`. Bounded, no heap growth.
- **Offline:** on `ST_HUB_OFFLINE`, show last list + age; hub resends full state on reconnect.

## 6. Hub: Registry, Sounds, Focus

- **Registry & state machine:** per session, `working ↔ attention ↔ waiting/waiting_queued
  → idle → (removed)`, resolved by the precedence in §2. `waiting` derives **only** from held
  `PermissionRequest`s — the current over-broad routing of every `Notification` into waiting is
  removed (Codex #2). The front prompt is mapped to its owning session via the
  `PermissionRequest` payload's `session_id`.
- **Timeouts (new, distinct from the permission cap):** `SESSION_IDLE_TTL` = **300 s** of no
  activity → `idle`; session removal reuses the existing reaper (~600 s silent). These are
  **separate** from the permission fail-closed cap whose docs need reconciling (§8) — do not
  reuse the 590 s prompt cap for session idle.
- **Branch:** computed via `git rev-parse` **async, off the `beacon.bridge` serial queue**,
  cached per `cwd`. Never shelled out on the hook-reply path (Codex #7) — a synchronous git
  spawn there would delay held `PermissionRequest` replies.
- **Sounds (hub-only, v1):** keep `beacon-prompt.wav` for `waiting`; add
  `beacon-attention.wav` for `attention`. **Debounce (Codex #8):** chime on the aggregate
  `0→>0` transition per state bucket; suppress same-session repeats within a **1.5 s** global
  window; a session re-entering a state it just left does not re-chime until it has left that
  state. Mutable via the existing prompt-mute pref.
  - **Amended 2026-07-18:** this bullet predates issue #110, which split `question` out of
    `attention` into its own state ranked *above* it. That silently removed the chime from asking
    sessions — they no longer reached the `attention` bucket. `question` now has its own bucket edge
    under the same aggregate `0→>0` rule, reusing `beacon-prompt.wav` (both mean "you are needed
    now"; `attention` means "the turn is done"). The row for `attention` below still reads "question
    or done; not differentiated" — that is now only true of the *sound*, not the state.
- **Host-context capture (Phase 2):** new `SessionStart` POST carries `$TERM_PROGRAM`,
  `$TERM_SESSION_ID`/`tty`, `pid`. Installer adds `clear|compact` matchers (today: only
  `startup|resume`). **Gated by a spike** (§7).
- **Focus resolver (Phase 2):** `open` → look up `s<id>` → tiered best-effort:
  iTerm2/Apple Terminal precise (AppleScript via `TERM_SESSION_ID`/`tty`); VS Code/Cursor
  `code -r <folder>` (right repo window, not the exact tab); Ghostty/Warp app-level. Must
  fail gracefully and **never block the hook-reply path** or hang on the macOS
  Automation/Accessibility permission prompt. **Gated by a spike** (§7).

## 7. Phases & Spikes

All three phases are committed. Spikes are de-risking steps *inside* their phase, not
off-ramps.

### Phase 1 — Session awareness (read-only)
Hub registry + state machine + sounds; additive `sessions` frame (caps + worst-case test);
device session-list screen across all 7 themes; `beacon-attention.wav`. No `open`, no host
capture, no branch focus. Low risk — all hook-sourced, all surfaces we control.

### Phase 2 — Tap-to-open
- **Spike first** (`docs/spikes/`): per host app (iTerm2, Apple Terminal, VS Code, Cursor,
  Ghostty, Warp) verify (a) what host context is actually reachable from a `SessionStart`
  hook subprocess's environment, and (b) that each focus tier works, including the
  Automation/Accessibility permission path failing gracefully.
- Then: `SessionStart` host-capture POST; `open` command (Protocol.swift + AppDelegate +
  hub_proto); device tap feedback; focus resolver; branch display.

### Phase 3 — Device audio
- **Spike first** (`docs/spikes/`): ES8311 + I2S chime under full BLE+TLS+LVGL load; prove
  the ≥60 KB internal-heap floor holds (transient min today ~53 KB; I2S DMA buffers add
  ~2-4 KB from the same internal pool TLS/BLE depend on).
- Then, only if the floor holds with margin: device-side chimes.

## 8. Pre-existing Cleanup (permission-cap docs only)

The **permission** fail-closed cap is documented inconsistently — `hub/CONTRACT.md` §B says
**25 s**, `docs/tech.md` §8 says **~30 s**, code uses **590 s** (`records.h:69`,
`ClaudeCodeBridge.swift:362`). Pick one reference and align docs + code. This is a separate
concern from the new `SESSION_IDLE_TTL` (§6, 300 s); the session TTL does **not** reuse the
permission cap, so this cleanup unblocks but does not redefine session timeouts.

## 9. Testing

- **Hub (`BeaconHubKit`):** registry state-machine table tests (each hook → expected state,
  including precedence overlaps from §2); standalone `sessions` frame encode/worst-case
  `< 1024 B`; debounce transitions (no stacked chimes across parallel sessions); `waiting` vs
  `waiting_queued` mapping to the front prompt's `session_id`; `s<n>` minting is monotonic /
  never reused.
- **Firmware (`native`):** `hub_proto` parse of the `sessions` frame (caps, truncation,
  malformed, unknown-field tolerance); `buddy_session_t[5]` fixed-size handling; sort-by-`ts`;
  0/1-session rendering; age suppressed when clock unsynced; tap-feedback pinning across
  re-sort (Phase 2).
- **Manual:** 3 parallel sessions across ≥2 host apps; verify glanceable state + correct
  chimes; Phase 2 focus per tier.

## 10. Risks

| Risk | Mitigation |
|---|---|
| Host-context not reachable from hook env | Phase 2 spike before committing; tap-to-open degrades to app-level focus |
| Device audio breaches heap floor | Phase 3 spike; stays hub-only if it doesn't hold |
| 7-theme layout variance for 4-row list | Cap at 4 rows; follow per-theme list-row pattern; per-theme visual QA |
| Combined status frame overflow (>1024 B silently dropped) | `sessions` is a standalone frame, not embedded in `buddy` (§4) |
| Old firmware blanks activity list on hub upgrade | `buddy`/`entries` frame unchanged; `sessions` is a separate frame old firmware ignores (§4) |

## Appendix A — Codex review resolutions

| Finding | Resolution |
|---|---|
| 6 rows don't fit (386 px safe height) | Cap visible rows at 4; document row height in DESIGN.md (§5) |
| `waiting` over-broad (all Notifications) | Derive `waiting` only from held PermissionRequests (§6) |
| Prompt-queue vs session coupling | `waiting` (front) vs `waiting_queued` (behind), mapped to active prompt (§2) |
| `open` in Phase 1 but spike in Phase 2 | Move `open` + tap feedback to Phase 2 with the spike (§7) |
| Sort reorders tap feedback | Pin feedback by session id, immune to re-sort (§5) |
| Git branch blocks hook reply | Async off bridge queue, cached per cwd (§6) |
| Chime debounce absent | `0→>0` per-bucket transition, 1.5 s same-session suppression (§6) |
| `entries` deprecation blanks old firmware | `sessions` is a separate frame; `buddy`/`entries` unchanged (§4) |
| Timeout doc drift (25/30/590 s) | Reconcile permission-cap docs; `SESSION_IDLE_TTL`=300 s is independent (§6, §8) |
| **Combined frame >1024 B silently dropped** | `sessions` moved to its own frame, not inside `buddy` (§4) |
| **`s<n>` reuse/rollover unspecified** | Monotonic `uint32`, never reused in a process lifetime (§3) |
| **`idle` TTL undefined** | `SESSION_IDLE_TTL` = 300 s (§6) |
| **`ended` ambiguous on wire** | Lifecycle-only — session removed, absent from next frame (§2) |
| **`working` trigger underspecified** | Statusline/tool activity + explicit precedence (§2) |
| **`ts` clock skew / unset RTC** | Device uses synced clock; suppresses age if unsynced (§5) |
| **0/1-session rendering unspecified** | Empty idle state + single centered row (§5) |
| Theme file vs catalog naming (calm↔dotmatrix) | Spec uses `*_<theme>.cpp` file/code names; catalog display names differ (`theme_catalog.h`) |
| List-row "component" is per-view helper, not shared API | Reworded to "follow the per-view list-row pattern" (§5) |
