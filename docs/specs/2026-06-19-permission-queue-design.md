# Permission Prompt Queue — Design

Date: 2026-06-19
Status: Approved (design); pending implementation plan
Scope: `firmware/` + `hub/` (touches the frozen BLE contract)

## Problem

The Coding Buddy gates one Claude Code permission prompt at a time. A second
concurrent prompt is auto-denied immediately
(`hub/Sources/beacon-hub/ClaudeCodeBridge.swift`, "another prompt is pending").
Anyone running parallel Claude Code sessions/agents silently loses prompts: the
device shows the first, every other in-flight permission is denied without ever
reaching the screen.

## Goal

Queue concurrent permission prompts instead of dropping them. Show them on the
device one at a time with a position counter; let the user clear them in order.

## Non-goals (YAGNI)

- Browsable/reorderable queue or deciding out of order (one-at-a-time only).
- Bulk "approve all / deny all".
- Per-prompt priority or grouping by session.

## The hard constraint (why "never expires" is impossible)

A permission prompt is a **held-open HTTP request** from Claude Code's hook. Its
deadline is owned by Claude Code, not the hub:

- The hook timeout is configured at **190s**
  (`hub/claude-code-settings.snippet.json`); the hub caps itself at **180s** to
  answer just under it (`ClaudeCodeBridge.swift`).
- Claude Code's `PermissionRequest` maximum is **600s**. The hub can *stretch*
  each prompt's runway toward that ceiling but cannot remove it: once Claude
  Code's deadline passes, it reclaims the request and the hub can no longer
  answer.

With one-at-a-time display, **all queued prompts' clocks run concurrently**. A
deep queue does not expire because it is deep — it expires because the user
cannot clear it before the shared wall clock runs out. Unbounded count does not
change that; only deciding fast enough (or a larger timeout) does.

```
t=0    A,B,C,D,E arrive   -> 5 clocks start, each ~600s
       device shows A (1 of 5)
t=40   approve A          -> B (1 of 4)
t=95   approve B          -> C (1 of 3)
...
       if E is reached after its deadline, E was already auto-denied
```

## Decisions

| # | Decision |
|---|----------|
| 1 | **One-at-a-time + counter.** Device shows the front prompt with a `(1 of N)` badge. |
| 2 | **Queue lives hub-side.** Device only ever holds the front prompt + a count; no array over BLE. |
| 3 | **Unbounded count.** Remove the "another prompt is pending" auto-deny. |
| 4 | **Raise hook timeout 190s -> ~600s** (`PermissionRequest` max) for maximum runway; hub cap follows (~590s). |
| 5 | **A prompt that expires while queued (not front) is auto-denied silently.** The count just decrements; no device note. |
| 6 | **FIFO, oldest-first.** Front = oldest = closest to its deadline, so front-first wastes the fewest. |

## Architecture (hub)

Today: `pending: [id: Pending]` keyed by minted id, plus a single `activeId`. A
second prompt is rejected at the `activeId != nil` guard.

Change: add an ordered FIFO `queue: [String]` of pending ids. `front =
queue.first` is the prompt published to the device. Each prompt keeps its **own**
cap timer, started on arrival (so all queued clocks run concurrently, matching
the wall-clock reality above). Remove the `activeId != nil` auto-deny.

| Event | Behavior |
|-------|----------|
| New permission arrives (past offline/quit/question checks) | mint id -> append to `queue` -> if now front, publish as `buddy.prompt`; else re-publish front with new `qlen` |
| User decides front | `finish(front)` -> remove from queue -> publish next front (or clear if empty) |
| **Queued** (non-front) prompt cap fires | `finish(capped)` **silently** -> remove from queue -> re-publish front with decremented `qlen` |
| **Front** (shown) prompt cap fires | keeps today's UX -> device shows `TOO LATE`, user dismisses (it is on screen, so not silent) |
| Answered on Mac (withdraw) | remove from queue at any position, decrement `qlen` |
| Hub quit drain | denies **all** queued prompts (`drainHeldPrompts` already iterates all non-done pending — works as-is) |

The published `buddy.prompt` always reflects the current front + current `qlen`.

The offline check is unchanged: a prompt that arrives while the device is
disconnected is still auto-denied "Beacon device offline" — the queue only holds
prompts that can actually be shown.

Note: `qlen` (prompt-queue depth) and `buddy.waiting` (sessions blocked on a
user decision) measure different things and both remain. They can differ — keep
them independent.

## Contract change (minimal — one field)

`buddy.prompt` gains **`qlen`** = total pending prompts including the shown
front. Position is always 1 (front is always shown), so no `qpos` field is
needed. This touches the frozen contract, so all of the following stay in sync
(see `hub/CONTRACT.md` framing):

- `hub/CONTRACT.md` — schema doc: add `qlen` to the `buddy.prompt` block.
- `hub/Sources/BeaconHubKit/Protocol.swift` — `BuddyPrompt` gains `qlen`.
- `firmware/src/core/hub_proto.cpp` — status parser fills `qlen`.
- `firmware/src/core/records.h` — `buddy_prompt_t` gains `uint8_t queue_len`.

`qlen` is optional/absent-safe: absent or `<=1` means a single prompt (no badge),
preserving back-compat with an un-updated peer.

## Device UI

Eyebrow badge: `PERMISSION -- APPROVE? (1 of 3)` when `queue_len > 1`; unchanged
when `<= 1`. After the user decides, the next prompt slides in immediately and
the counter decrements — **the decrementing badge is the confirmation**, so the
2s `SENT_OK` confirm beat is skipped when more prompts wait. Renders across all
7 buddy views (`firmware/src/ui/screens/views/buddy_*.cpp`).

## Integration gotchas (verify during implementation)

1. **Device-local expiry must move too.** `BUDDY_PROMPT_EXPIRY_S = 180`
   (`firmware/src/core/records.h`) would fire `TOO LATE` at 180s even though the
   hub now holds for ~600s. Raise it to align (~590s) or the device prematurely
   gives up on the front prompt. Keep a local expiry (fail-safe for a dropped
   hub link), just aligned to the new cap.

2. **State reset on prompt swap.** When the front advances, the device receives a
   new `prompt.id`; `decision_state` must reset to `PROMPT_IDLE_DECISION` so the
   next prompt is immediately tappable. Already handled at `hub_proto.cpp:83-84`
   (the `same_prompt` check); locked with a test.

3. **`waitingSessions` must become a refcount.** It is a `Set<String>` today; with
   a queue, two prompts from one session would let the first `finish` zero the
   session's wait while another is still queued. Use `waitingSessionCounts:
   [String: Int]`.

4. **Front-only decisions.** The device only shows the front, so `resolve(id:)`
   must reject any live non-front id (out-of-order guard). Queued-expiry is a
   hub-internal `finish(capped:)`, never a device decision.

5. **Bounded late-ack tombstone.** Don't prune resolved `pending` entries on every
   enqueue (queuing makes that erase a just-resolved id and degrade a racing late
   decision from `.late` to `.unknown`); GC them on a short TTL in the reaper.

6. **`qlen` parse must clamp.** `(uint8_t)(p["qlen"] | 1)` leaves `0`, wraps `256`
   to `0`, and mishandles negatives — none mean "single". Read as int, clamp
   `<1 -> 1`, `>255 -> 255`.

7. **Naming.** The bridge already has `private let queue = DispatchQueue`; the new
   FIFO must be `promptQueue`, not `queue` (compile collision).

8. **Stale prose.** `CONTRACT.md` §C.3/§D and `tech.md` §8/§9 still describe the
   ~25s window and concurrent-auto-deny; update them, not just the §A frame.

(Gotchas 3-8 surfaced in the Codex plan review, 2026-06-19, and are carried in the
implementation plan.)

## Testing

Hub (`BeaconHubKit` / `beacon-hub` tests):
- Enqueue 3 -> decide front -> assert advance to next + `qlen` decrements.
- Expire a *queued* (non-front) prompt -> assert silent removal + `qlen`
  decrement, front unchanged.
- Quit drain -> all queued prompts denied.
- Withdraw a middle prompt -> removed, `qlen` decrements, front unchanged.

Firmware (`native` env, table-driven where applicable):
- `hub_proto` parses `qlen`; absent/`<=1` -> no badge.
- Badge renders only when `queue_len > 1`.
- `decision_state` resets to idle on `prompt.id` change.

## Files touched (summary)

- `hub/Sources/beacon-hub/ClaudeCodeBridge.swift` — queue, advance, silent
  expiry, timeout bump.
- `hub/Sources/BeaconHubKit/Protocol.swift` — `qlen` on `BuddyPrompt`.
- `hub/claude-code-settings.snippet.json` — hook timeout 190 -> ~600.
- `hub/CONTRACT.md` — document `qlen`.
- `firmware/src/core/records.h` — `queue_len`; raise `BUDDY_PROMPT_EXPIRY_S`.
- `firmware/src/core/hub_proto.cpp` — parse `qlen`.
- `firmware/src/ui/screens/views/buddy_*.cpp` — `(1 of N)` badge.
- Tests on both sides.
