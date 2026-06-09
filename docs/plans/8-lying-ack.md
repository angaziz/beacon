# Implementation Plan: Fix "Lying ack & late-approve trap" (#8)

## 1. Root-cause summary (exact file:line)

**The lie (Step 1 target):**
- `AppDelegate.swift:90-91` — `handle(_:)` calls `bridge?.resolve(id:approve:)` then **unconditionally** `central.send(HubAck.ack(id: id, ok: true))`. The ack is sent regardless of whether resolve actually applied.
- `ClaudeCodeBridge.swift:60-62` — `resolve` is fire-and-forget (`queue.async`, returns `Void`); the caller can't know the outcome.
- `ClaudeCodeBridge.swift:184-193` — `finish` no-ops when `pending[id]` is missing or already `done` (`guard let p = pending[id], !p.done else { return }`). The 25s cap (`:171`) calls `finish(... capped:true)` and sets `done=true`; a later device approve hits the guard and silently does nothing — yet the hub already told the device `ok:true`.

**The premature clear (Step 2 target):**
- `buddy_hud.cpp:28-36` — `decide()` clears the prompt (`r.prompt.present = false; ds_set_buddy(&r)`) the instant `hub_send_permission` returns true. `hub_send_permission` (`hub_task.cpp:53-61`) returns the result of `g_link->send()`, which per `hublink.h:19-23` is **only local enqueue acceptance**, explicitly "NOT the application-level ack." So the prompt vanishes on enqueue, never waiting for the real ack.

**The unwired ack (Step 2 target):**
- `hub_task.cpp:17-25` — `on_frame` routes every inbound frame through `hub_parse_status` only. `hub_parse_ack` (`hub_proto.cpp:108-127`) is fully implemented and unit-tested (`test_hub_proto/test_main.cpp:187-209`) but **never called** anywhere in firmware. Inbound ack/err frames currently parse as "bad/ignored frame" (`hub_task.cpp:21` LOGW) and are dropped.

## 2. Wire-format / contract decision

**No contract change needed. The frozen contract already represents all three outcomes.**

- `hub/CONTRACT.md` §B and `tech.md` §7.1 already define: `{"v":1,"ack":"p07","ok":true}`, and `ok` is a boolean (so `ok:false` is in-contract), plus `{"v":1,"err":"unknown_prompt_id","id":"p07"}`.
- Swift side already has both encoders: `HubAck.ack(id:ok:)` and `HubAck.err(id:reason:)` (`Protocol.swift:115-127`).
- Device side already parses both: `hub_parse_ack` fills `hub_ack_t{ id, ok, is_err }` (`hub_proto.h:49`, `hub_proto.cpp:108-127`), distinguishing ok-true / ok-false / err.

Fix is purely **wiring existing, contract-blessed shapes** — Swift must choose `ack(ok:false)`/`err` instead of always `ack(ok:true)`, and the device must consume the ack that already arrives. No new wire fields, no schema bump.

Decision on which negative shape (Swift, Step 1): emit `ack(ok:false)` for the late/already-resolved case and `err(reason:"unknown_prompt_id")` for an id never seen.

**Codex review caveat (addressed):** the frozen contract docs (`hub/CONTRACT.md:31`, `docs/tech.md:175`) currently only *illustrate* `ack(ok:true)`; `ok` is a bool so `ok:false` is parseable (`hub_proto.cpp:112-117`, `Protocol.swift:115-127`) but never documented. Since `ok:false` carries real semantics now, **explicitly document `ok:false` = "decision did not apply (late/superseded)"** in both `hub/CONTRACT.md` and `docs/tech.md` §7.1. This is a doc clarification of an already-parseable field — no wire/schema change.

## 3. Ordered, surgical edit list

### STEP 1 — Mac/Swift (fix the lying ack FIRST)

**Edit 1.1 — `ClaudeCodeBridge.swift`: make resolve report outcome.**
- Change `resolve(id:approve:)` to return a `Bool`/reason (applied?) by running `finish` on `queue` **synchronously** (`queue.sync`) and propagating `finish`'s result. Keep "safe to call from any thread."
- Change `finish(id:approve:capped:)` (`:184-193`) to return its outcome: not-applied at the existing guard (`pending[id]` missing or `done`), applied after it resolves and calls `p.respond`.
- Distinguish *unknown id* (`pending[id]==nil`) from *late/already-done* (`pending[id]?.done==true`) to pick err vs ack(ok:false).
- The 25s cap path (`:171`) and `setEventHandler` already call `finish`; they ignore the return value — fine.

**Edit 1.2 — `AppDelegate.swift:89-92`: send the truthful ack.**
- Switch on resolve's result: applied → `ack(ok:true)`; known-but-late → `ack(ok:false)`; unknown id → `err(reason:"unknown_prompt_id")`.
- Nil-bridge case must also be non-lying (send err/ack(ok:false), not ok:true).

**Step 1 verification:** extend `ProtocolTests.swift` with an `ok:false` shape assertion; `cd hub && swift test`.

### STEP 2 — Device/C++ (wire the real ack; stop clearing on enqueue)

**Codex review caveat (addressed):** the enqueue-clear bug is **duplicated across all 7 buddy views** (`buddy_hud.cpp:28-36`, `buddy_calm.cpp:22-30`, `buddy_editorial.cpp:9-17`, `buddy_led.cpp:21-28`, `buddy_analog.cpp:25-32`, `buddy_blueprint.cpp:23-30`, `buddy_oscilloscope.cpp:27-35`). Fixing only `buddy_hud` would leave the lie intact on every other theme. **Centralize the decide path** so the fix lands once, then render the new states per-view.

**Edit 2.1 — `records.h`: add device-local prompt confirm-state field** to `buddy_prompt_t` (`uint8_t decision_state;` with `PROMPT_IDLE_DECISION=0`, `PROMPT_PENDING=1`, `PROMPT_SENT_OK=2`, `PROMPT_TOO_LATE=3`). Memset-zero defaults to IDLE. Device-local, never serialized — document as not-on-wire (confirmed: `hub_proto.cpp:74-83` populates fields individually, commands serialize only `id/decision` `hub_proto.cpp:98-105`).

**Edit 2.2 — Centralize decide into `hub_task.cpp` (declared in `hub_task.h`): new `bool buddy_decide(bool approve)`.** Reads buddy rec, applies the canonical guard (`prompt.present` && state not `ST_HUB_OFFLINE`/`ST_RECONNECTING` && `decision_state == PROMPT_IDLE_DECISION`), calls `hub_send_permission`; on success sets `decision_state = PROMPT_PENDING` and `ds_set_buddy` (does **not** clear `present`); returns whether the decision was enqueued. This folds in the old per-view guard + the re-tap guard. Place near `hub_send_permission`/`apply_ack` since it owns the prompt lifecycle.

**Edit 2.3 — Repoint all 7 buddy views' decide callbacks to `buddy_decide(approve)`.** Delete each view's local `send → present=false → ds_set_buddy` body; the callback just calls `buddy_decide`. Fixes enqueue-clear everywhere (AC3) in one place.

**Edit 2.4 — `hub_task.cpp` `on_frame` (`:17-25`): dispatch acks before status, via new `apply_ack`.** Try `hub_parse_ack` first; if it parses, route to `apply_ack` and return; else fall through to `hub_parse_status`.
- `apply_ack`: load buddy rec; if `prompt.present` && id matches && `decision_state==PROMPT_PENDING`:
  - `ack.ok && !ack.is_err` → `PROMPT_SENT_OK`, clear prompt (`present=false`).
  - `!ack.ok` or `ack.is_err` → `PROMPT_TOO_LATE`, keep `present=true`.
  - mismatched/stale id → ignore (log only).

**Edit 2.5 — Render the confirm states in ALL 7 buddy views' `update()`** inside each existing prompt box (theme-consistent styling): `PROMPT_PENDING` → dim/disable both actions + eyebrow "SENT — AWAITING…"; `PROMPT_TOO_LATE` → eyebrow "TOO LATE — DIDN'T APPLY" in the theme's down/negative color + a single dismiss affordance; default → existing approve/deny. Keep each view's bespoke look; only branch on `decision_state`. Minimum bar for every view: a TOO_LATE prompt must never render as a success/clear.

**Step 2 verification:** extend `test_hub_proto/test_main.cpp` with an `ok:false` case (parses true, `is_err=false`, `ok=false`). Extract `apply_ack`'s pure id-match/state-transition into a testable helper if it stays thin. Run native tests (`pio test -e native` or the configured env).

## 4. Verify each step
- Step 1: `cd hub && swift test`.
- Step 2: native unit tests for `hub_parse_ack` (incl. ok:false) + pure state-transition helper.
- Manual on-device late-approve race: trigger prompt → wait past 25s cap → tap Approve → expect "TOO LATE — DIDN'T APPLY" (not success). Positive path: approve within window → pending → clears on ok:true. Confirm prompt no longer vanishes on tap.

## 5. Risks / edge cases
- 25s cap race: `finish`'s `done` guard makes resolve idempotent; loser reports not-applied → `ack(ok:false)`.
- Unknown id → `err(unknown_prompt_id)`; device tolerates ack whose id ≠ current prompt (ignore).
- Reconnect mid-decision: full-frame resend on reconnect either omits the resolved prompt (clears stale PENDING) or re-shows it fresh (decision_state reset) for re-decide.
- Struct-size: adding `decision_state` to `buddy_prompt_t` — confirm nothing serializes raw struct; call out in PR (records.h carries frozen capacity macros).
- Double-tap while PENDING: mitigated by decide() guard.
- Order discipline: Step 1 must precede Step 2.
