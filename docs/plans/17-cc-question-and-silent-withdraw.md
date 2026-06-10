# Plan 17 — CC questions are not approve/deny; silent-withdraw when answered on Mac

## Problem (two reported device bugs)

1. **"approve" button on a Claude question.** When Claude uses `AskUserQuestion`, the
   `PermissionRequest` hook fires, the hub holds it as a binary approve/deny `BuddyPrompt`,
   and the device renders `deny | approve >`. A question is not yes/no, and a
   `PermissionRequest` hook can only allow/deny/ask anyway (it cannot pick an option).
   Worse: a held question occupies the **single** prompt slot
   (`ClaudeCodeBridge.handlePermission`: `if activeId != nil { auto-deny-busy }`), so a
   question can auto-reject a *real* permission that arrives while it sits there.

2. **"too late - didn't apply" after the user answers on the Mac.** The held permission is
   resolved in the Mac terminal; CC moves on, but nothing tells the hub. The hub keeps
   holding its orphaned HTTP connection; the device keeps showing the prompt; at 180s the
   device-local tick (`ds_tick_buddy_prompt`, datastore.cpp:128-129) flips
   `PROMPT_IDLE_DECISION -> PROMPT_TOO_LATE`, rendered as "too late - didn't apply"
   (buddy_calm.cpp:188). The message is false — the answer *did* apply, on the Mac.

## Decisions (from user)

- Questions: **indicate only**, no on-device answering.
- Resolved-on-Mac: **withdraw silently** (primary), because a stale question/permit must not
  block other real permissions (which have their own timeout and may auto-reject).

## Fix A — AskUserQuestion becomes a passive indicator (hub-only)

`ClaudeCodeBridge.handlePermission`, immediately after the `terminating` check (a question
needs no device and must never take the slot):

```
if tool == "AskUserQuestion" {
    respondDecision(conn, allow: true, event: event)   // let CC ask the human on the Mac
    if deviceConnected {
        buddy.entries = Array((["<HH:mm> <[dir]> asking a question"] + buddy.entries).prefix(3))
        publishBuddy()
    }
    log(id: "-", decision: "question-passthrough")
    return
}
```

- No `mintId`, no `pending`, no `activeId` — slot stays free.
- Returns `allow`, so AskUserQuestion runs = presents its options to the human in the
  terminal. (Allowing the tool is "yes, ask the human"; it does not auto-answer.)
- Indicator = one activity entry, rendered by the existing idle-entries UI. **No firmware
  change.**

Open check (verify in test): confirm CC reports `tool_name == "AskUserQuestion"` in the
`PermissionRequest` body, and that `allow` shows the question rather than auto-resolving it.

## Fix B — silent withdraw when the held connection closes (hub-only)

The held permission's `NWConnection` is parked after `route()` with no outstanding read. Add
a close watcher; if the peer (CC) closes while the prompt is still held, it means CC resolved
the dialog elsewhere (Mac) and abandoned the hook.

1. After parking the prompt in `handlePermission`, call `watchForClose(conn, id: shortId)`:

```
private func watchForClose(_ conn: NWConnection, id: String) {
    conn.receive(minimumIncompleteLength: 1, maximumLength: 1) { [weak self] _, _, isComplete, error in
        guard let self else { return }
        if isComplete || error != nil {
            self.queue.async { self.withdraw(id: id) }   // hop to queue; peer closed
        }
    }
}
```

2. New `withdraw(id:)` — like `finish` but no `respond`, no deny log, frees the slot:

```
private func withdraw(id: String) {
    guard let p = pending[id], !p.done else { return }   // our own send/cancel sets done first
    p.done = true
    p.timeout.cancel()
    if activeId == id { activeId = nil }                  // free the slot immediately
    if !p.sessionId.isEmpty, waitingSessions.remove(p.sessionId) != nil { buddy.waiting = waitingSessions.count }
    if buddy.prompt?.id == id { buddy.prompt = nil }
    publishBuddy()                                        // device clears the prompt, silently
    log(id: id, decision: "withdrawn-resolved-elsewhere")
}
```

- `done` guard prevents double-handling: when WE resolve (device decision or cap), `finish`
  sets `done=true` then `conn.cancel()`, which fires the watcher with an error — but `done`
  is already set, so `withdraw` is a no-op. Withdraw only wins when the peer closes first.
- Device side needs no change: `buddy.prompt=nil` -> status frame -> `hub_parse_status`
  sets `present=false` -> prompt clears. No "too late".

## Verification (requires the user + device + a live CC session)

- **Fix A:** ask Claude something that triggers `AskUserQuestion`. Device must NOT show
  approve/deny; it shows "asking a question" in the feed; the question is answerable on the
  Mac; a real Bash/Edit permission raised right after is still held (not auto-denied-busy).
- **Fix B (the empirical unknown):** raise a real Bash permission (device shows approve),
  answer it in the Mac terminal, watch hub stderr. If
  `decision=withdrawn-resolved-elsewhere` logs and the device clears silently -> confirmed.
  If it does NOT log, CC keeps the socket open and silent-withdraw is impossible via this
  signal -> fall back to honest wording on the device-local expiry (replace "too late -
  didn't apply" with "expired - may be answered on Mac") and revisit.

## Scope

- `hub/Sources/beacon-hub/ClaudeCodeBridge.swift` — Fix A branch, `watchForClose`, `withdraw`.
- No `Protocol.swift` change (reuses `allow`).
- No firmware change (indicator via existing entries; withdraw via existing prompt-absent path).
- Tests: a hub unit test for the AskUserQuestion passthrough (slot stays free) if the bridge
  is testable without the Network stack; the withdraw close-path is integration-only (manual).
```
