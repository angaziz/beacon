# Permission Prompt Queue Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Queue concurrent Claude Code permission prompts instead of auto-denying the second one, showing them on the device one at a time with a `(1 of N)` counter.

**Architecture:** The queue lives entirely hub-side. `ClaudeCodeBridge` keeps a FIFO of held prompts; the device only ever receives the *front* prompt plus a new `qlen` count. Deciding the front advances the queue and publishes the next. Each held prompt races its own Claude-Code-owned deadline, so a prompt that expires while still queued is auto-denied silently (count decrements).

**Tech Stack:** Swift 6 (SwiftPM, `BeaconHubKit` + `beacon-hub`), C++ firmware (PlatformIO `native` Unity tests), newline-delimited JSON over BLE.

Design: `docs/specs/2026-06-19-permission-queue-design.md` — Issue: #98

## Global Constraints

- **Conventional Commits**, per-component scope: `feat(hub): ...`, `feat(firmware): ...`. Subject lowercase imperative, no trailing period. Keep `BLE`/`CC`/`IMU` casing.
- **USER runs every `git commit`.** The "Commit" steps below give the exact message to use; do not run `git commit` yourself — stage and hand off.
- **ASCII only** in code and docs (use `--`, `=>`, not unicode arrows).
- **Frozen contract sync (4 places must agree):** `hub/CONTRACT.md` §A, `hub/Sources/BeaconHubKit/Protocol.swift`, `firmware/src/core/hub_proto.cpp`, `firmware/src/core/records.h`. Any wire change touches all four + their tests. **Also** update the prose timing/policy text: `CONTRACT.md` §C.3/§D ("~25s window", "auto-deny a second concurrent") and `docs/tech.md` §8/§9 — they currently describe the old single-prompt/35s policy.
- **`waitingSessions` becomes a refcount.** Today it is a `Set<String>`; with a queue, two prompts from one session must not let the first `finish` zero the session's wait. Use `waitingSessionCounts: [String: Int]` and set `buddy.waiting = waitingSessionCounts.count`.
- **Front-only decisions.** The device only ever shows the front; `resolve(id:approve:)` MUST reject any id that is not `queue.first` (return `.late` if the id is a known-but-resolved tombstone, else `.unknown`). Queued-expiry is hub-internal (`finish(capped:)`), never a device decision.
- **`qlen` is an additive `v:1` extension:** absent or `<=1` means a single prompt (no badge). Single-prompt frames stay byte-identical to today (omit `qlen`) so existing fixtures do not churn.
- **Pinned toolchain — do not bump.** Build firmware via `~/.beacon-pio/bin/pio`, hub via `swift`.
- **Hook timeout ceiling is Claude Code's, not ours:** `PermissionRequest` max is 600s. New hub cap is 590s (10s margin under a 600s hook timeout); device-local expiry aligns to 590s.

---

### Task 1: Wire field — `qlen` on the Swift prompt + contract doc

Adds the new optional field at the Swift edge and documents it. Optional + omitted-when-`<=1` keeps single-prompt frames identical to today.

**Files:**
- Modify: `hub/Sources/BeaconHubKit/Protocol.swift:29-34` (`BuddyPrompt`)
- Modify: `hub/CONTRACT.md:20-24` (§A status-frame example + note)
- Test: `hub/Tests/BeaconHubKitTests/` (add a `qlen` encoding case to the existing StatusFrame/Protocol test file)

**Interfaces:**
- Produces: `BuddyPrompt(id: String, tool: String, hint: String, qlen: Int? = nil)` — `qlen` encodes only when non-nil; callers pass `nil` for a lone prompt, `count` (>=2) when others wait.

- [ ] **Step 1: Write the failing test**

In the existing BeaconHubKit protocol test file, add:

```swift
func testBuddyPromptOmitsQlenWhenSingle() throws {
    let frame = StatusFrame(buddy: BuddyState(prompt: BuddyPrompt(id: "p1", tool: "Bash", hint: "ls", qlen: nil)))
    let json = String(data: try frame.encoded(), encoding: .utf8)!
    XCTAssertFalse(json.contains("qlen"), "lone prompt must not emit qlen (back-compat)")
}

func testBuddyPromptEmitsQlenWhenQueued() throws {
    let frame = StatusFrame(buddy: BuddyState(prompt: BuddyPrompt(id: "p1", tool: "Bash", hint: "ls", qlen: 3)))
    let json = String(data: try frame.encoded(), encoding: .utf8)!
    XCTAssertTrue(json.contains("\"qlen\":3"), "queued prompt must emit qlen")
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd hub && swift test --filter BuddyPromptOmitsQlenWhenSingle`
Expected: FAIL — `BuddyPrompt` has no `qlen` argument (compile error).

- [ ] **Step 3: Add the field**

Replace `BuddyPrompt` (lines 29-34):

```swift
public struct BuddyPrompt: Codable, Equatable {
    public var id: String
    public var tool: String
    public var hint: String
    public var qlen: Int?   // total pending prompts incl. this front one; nil/<=1 => lone prompt (omitted)
    public init(id: String, tool: String, hint: String, qlen: Int? = nil) {
        self.id = id; self.tool = tool; self.hint = hint; self.qlen = qlen
    }
}
```

`JSONEncoder` omits a nil optional, so single-prompt frames stay byte-identical.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd hub && swift test --filter BuddyPrompt`
Expected: PASS (both cases).

- [ ] **Step 5: Update the contract doc**

In `hub/CONTRACT.md` §A, change the example prompt (line 22) and add a bullet. New prompt line:

```json
          "prompt":{"id":"p07","tool":"Bash","hint":"rm -rf /tmp/build","qlen":2}}}
```

Add under the §A bullets (after line 24):

```markdown
- `buddy.prompt.qlen` (additive `v:1` ext, issue #98) — total pending prompts incl. the shown front.
  Omitted or `<=1` => a single prompt (no `(1 of N)` badge). The device always shows the front;
  position is implicitly 1, so there is no `qpos`.
```

Also update the stale **prose** that still describes the single-prompt / short-timeout policy (grep each, they are now wrong):
- `hub/CONTRACT.md` §C.3 / §D: "~25s window" and "cap => deny" timing, and any "auto-deny a second concurrent prompt" policy line — replace with: prompts now **queue** (FIFO, one shown at a time, `qlen` badge); hold is ~590s under a 600s hook timeout; only a queued prompt's own cap expiry denies it (silently).
- `docs/tech.md` §8 (FR-BUDDY) / §9: the 25s/35s decision-window and concurrent-deny wording -> the ~590s hold + queue policy. If §7.1 inlines the buddy frame (grep `"hint"`), add `"qlen"` there too with the §A note; if it only points to CONTRACT.md, no frame edit needed.
- `hub/claude-code-settings.snippet.json` `_comment` (Task 3 Step 5) — already covered there.

- [ ] **Step 6: Commit (USER runs)**

```bash
git add hub/Sources/BeaconHubKit/Protocol.swift hub/CONTRACT.md hub/Tests/BeaconHubKitTests docs/tech.md
git commit -m "feat(hub): add qlen field to buddy prompt frame"
```

---

### Task 2: Hub queue core — FIFO of held prompts, advance on decide

Replaces the single `activeId` + "another prompt is pending" auto-deny with a FIFO queue. The front is what the device sees; deciding it advances to the next.

**Files:**
- Modify: `hub/Sources/beacon-hub/ClaudeCodeBridge.swift` — `Pending` (55-65), state (67/77), `handlePermission` (274-349), `finish` (351-372). (Line refs are vs HEAD `baec8e7`; PR #99 shifted the back half ~+13 from the earlier draft — the quoted code is unchanged.)
- Test: `hub/Tests/beacon-hubTests/ClaudeCodeBridgeTests.swift`

**Interfaces:**
- Consumes: `BuddyPrompt(..., qlen:)` from Task 1.
- Produces: `private var queue: [String]` (FIFO of minted ids; `queue.first` = front); `Pending` now carries `tool`/`hint`; `private func publishFront()` re-derives `buddy.prompt` from the front + `queue.count` and publishes.

- [ ] **Step 1: Write the failing test**

The bridge holds prompts on a private serial queue and answers HTTP, which is awkward to unit-test directly. Drive it through the public seam the tests already use (`resolve(id:approve:)` + the `onBuddyUpdate` callback). Add:

```swift
func testSecondPromptQueuesInsteadOfDenying() {
    let bridge = ClaudeCodeBridge()
    bridge.setDeviceConnected(true)
    var published: [BuddyState] = []
    bridge.onBuddyUpdate = { published.append($0) }

    bridge.injectPermissionForTest(toolUseId: "t1", tool: "Bash", hint: "one")
    bridge.injectPermissionForTest(toolUseId: "t2", tool: "Write", hint: "two")

    let front = published.last!.prompt!
    XCTAssertEqual(front.hint, "one", "front stays the first prompt")
    XCTAssertEqual(front.qlen, 2, "qlen reflects both pending")
}

func testDecidingFrontAdvancesToNext() {
    let bridge = ClaudeCodeBridge()
    bridge.setDeviceConnected(true)
    var published: [BuddyState] = []
    bridge.onBuddyUpdate = { published.append($0) }
    bridge.injectPermissionForTest(toolUseId: "t1", tool: "Bash", hint: "one")
    bridge.injectPermissionForTest(toolUseId: "t2", tool: "Write", hint: "two")

    let frontId = published.last!.prompt!.id
    _ = bridge.resolve(id: frontId, approve: true)

    let next = published.last!.prompt!
    XCTAssertEqual(next.hint, "two", "front advanced to the second prompt")
    XCTAssertNil(next.qlen, "only one left => qlen omitted")
}
```

This needs a test seam. Add to `ClaudeCodeBridge` (internal, test-only — mirrors the existing `handleStatusline` internal seam):

```swift
// Test seam: run the held-prompt path without the Network/HTTP stack. respond is a no-op sink.
func injectPermissionForTest(toolUseId: String, tool: String, hint: String) {
    queue.sync {
        self.enqueuePrompt(respond: { _, _, onSent in onSent?() },
                           sessionId: toolUseId, tool: tool, hint: hint)
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd hub && swift test --filter testSecondPromptQueues`
Expected: FAIL — `injectPermissionForTest`/`enqueuePrompt` undefined.

- [ ] **Step 3: Extend `Pending` to carry display data**

Replace `Pending` (lines 55-65) — add `tool`/`hint` so a queued prompt can be (re)published when it reaches the front:

```swift
private final class Pending {
    let respond: (Bool, String?, (() -> Void)?) -> Void
    var done = false
    var resolvedAt: Date?   // set when done; lets the reaper bound the late-ack tombstone (see Task 3)
    let timeout: DispatchSourceTimer
    let sessionId: String
    let tool: String
    let hint: String
    init(respond: @escaping (Bool, String?, (() -> Void)?) -> Void, timeout: DispatchSourceTimer,
         sessionId: String, tool: String, hint: String) {
        self.respond = respond; self.timeout = timeout; self.sessionId = sessionId
        self.tool = tool; self.hint = hint
    }
}
```

Also replace the `waitingSessions: Set<String>` field (line 77) with a refcount map so two prompts from one session are counted independently:

```swift
private var waitingSessionCounts: [String: Int] = [:]   // sessionId => # of its prompts blocked on a user decision
```

- [ ] **Step 4: Replace `activeId` with a FIFO queue**

Replace line 67 (`private var activeId: String?`) with — **note the name**: the bridge already has `private let queue = DispatchQueue(...)` at line 32, so the FIFO MUST NOT be called `queue` (it would collide / fail to compile). Name it `promptQueue`:

```swift
private var promptQueue: [String] = []   // FIFO of minted ids; promptQueue.first = the prompt shown on the device.
```

Add a front-publish helper (near `publishBuddy`, ~line 520):

```swift
// Re-derive the device-visible prompt from the front of the FIFO + current depth, then publish.
private func publishFront() {
    if let frontId = promptQueue.first, let p = pending[frontId], !p.done {
        let n = promptQueue.count
        buddy.prompt = BuddyPrompt(id: frontId, tool: p.tool, hint: p.hint, qlen: n > 1 ? n : nil)
    } else {
        buddy.prompt = nil
    }
    publishBuddy()
}
```

- [ ] **Step 5: Factor the enqueue path out of `handlePermission`**

In `handlePermission`, delete the busy auto-deny (the `if activeId != nil { ... "another prompt is pending" }` block, ~lines 320-325) entirely. Replace the prompt-creation block (~lines 327-349, from the `waitingSessions.insert` through `onPromptArrived?()`) with a call into a new `enqueuePrompt`, and add that method:

```swift
        // (inside handlePermission, replacing lines 314-335)
        enqueuePrompt(respond: { [weak self] allow, msg, onSent in
                          self?.respondDecision(conn, allow: allow, event: event, message: msg, onSent: onSent)
                      },
                      sessionId: sid ?? "", tool: tool, hint: hint)
        watchForClose(conn, id: promptQueue.last!)   // peer closes => answered on the Mac => withdraw (issue #17)
```

```swift
// Append a held prompt to the FIFO and publish the (possibly unchanged) front with the new depth.
// Each prompt races its own 590s cap (under the 600s hook timeout); see finish/timeout handling.
private func enqueuePrompt(respond: @escaping (Bool, String?, (() -> Void)?) -> Void,
                           sessionId: String, tool: String, hint: String) {
    if !sessionId.isEmpty {
        waitingSessionCounts[sessionId, default: 0] += 1
        buddy.waiting = waitingSessionCounts.count
    }
    let shortId = mintId()
    let cap = DispatchSource.makeTimerSource(queue: queue)
    cap.schedule(deadline: .now() + 590)   // fail-closed, just under the 600s hook timeout (Task 3)
    cap.setEventHandler { [weak self] in self?.finish(id: shortId, approve: false, capped: true) }
    // Do NOT blanket-prune done tombstones here: with queuing, enqueues are frequent and would erase a
    // just-resolved id's tombstone, degrading a racing late device decision from .late to .unknown. The
    // reaper (Task 3) drops tombstones on a TTL instead.
    pending[shortId] = Pending(respond: respond, timeout: cap, sessionId: sessionId, tool: tool, hint: hint)
    promptQueue.append(shortId)
    cap.resume()
    publishFront()
    log(id: shortId, decision: "prompt")
    onPromptArrived?()
}
```

Note: `makeTimerSource(queue: queue)` above uses the dispatch `queue` (the serial actor queue) — correct. The FIFO is `promptQueue` (Step 4). `watchForClose` reads `promptQueue.last!` at the call site (the just-appended id).

- [ ] **Step 6: Advance the queue in `finish`**

In `finish` (~351-372), after `p.done = true` add `p.resolvedAt = Date()` (stamps the tombstone for the reaper), and replace the `activeId`/`buddy.prompt` clearing (the `if activeId == id`, `waitingSessions.remove`, and `if buddy.prompt?.id == id` lines, ~360-367) with FIFO removal + refcount decrement + front republish:

```swift
        p.resolvedAt = Date()   // tombstone stamp; the reaper (Task 3) drops it after the late-ack TTL
        promptQueue.removeAll { $0 == id }
        if !p.sessionId.isEmpty, let n = waitingSessionCounts[p.sessionId] {
            if n <= 1 { waitingSessionCounts.removeValue(forKey: p.sessionId) }
            else { waitingSessionCounts[p.sessionId] = n - 1 }
            buddy.waiting = waitingSessionCounts.count
        }
        publishFront()   // advances to the next front, or clears to idle when the FIFO is empty
```

(Delete the old `if activeId == id { activeId = nil }` and `if buddy.prompt?.id == id { buddy.prompt = nil }; publishBuddy()` lines — `publishFront` subsumes them. Keep the `guard !p.done else { return .late }` and the kept-tombstone comment so a late device decision still reports `.late`.)

- [ ] **Step 7: Run tests to verify they pass**

Run: `cd hub && swift test --filter ClaudeCodeBridge`
Expected: PASS, including the two new cases. Fix any existing test that asserted the old "another prompt is pending" deny (it is now queued) — update it to assert queuing.

- [ ] **Step 8: Commit (USER runs)**

```bash
git add hub/Sources/beacon-hub/ClaudeCodeBridge.swift hub/Tests/beacon-hubTests
git commit -m "feat(hub): queue concurrent permission prompts instead of auto-denying"
```

---

### Task 3: Hub — silent queued-expiry, longer hold, all-queue drain/withdraw

Raises the hold to ~600s, makes a queued (non-front) expiry silent, and confirms drain/withdraw operate across the whole queue.

**Files:**
- Modify: `hub/Sources/beacon-hub/ClaudeCodeBridge.swift` — `withdraw` (~389-400), the cap comment (now in `enqueuePrompt`; old `+ 180` was at line 331)
- Modify: `hub/claude-code-settings.snippet.json:9` (190 -> 600)
- Modify: `hub/Tests/BeaconHubKitTests/HooksDetectionTests.swift:14` (190 -> 600), and any Swift source hardcoding 190
- Test: `hub/Tests/beacon-hubTests/ClaudeCodeBridgeTests.swift`

**Interfaces:**
- Consumes: `queue`, `pending`, `publishFront()` from Task 2.

- [ ] **Step 1: Write the failing tests**

```swift
func testQueuedExpirySilentlyDropsAndKeepsFront() {
    let bridge = ClaudeCodeBridge()
    bridge.setDeviceConnected(true)
    var published: [BuddyState] = []
    bridge.onBuddyUpdate = { published.append($0) }
    bridge.injectPermissionForTest(toolUseId: "t1", tool: "Bash", hint: "front")
    bridge.injectPermissionForTest(toolUseId: "t2", tool: "Write", hint: "behind")

    let behindId = bridge.queuedIdsForTest().last!   // the non-front one
    bridge.expirePromptForTest(id: behindId)          // simulate its 590s cap firing (hub-internal deny)

    let front = published.last!.prompt!
    XCTAssertEqual(front.hint, "front", "front prompt unchanged by a queued expiry")
    XCTAssertNil(front.qlen, "only the front remains => qlen omitted")
}

func testResolveRejectsNonFrontId() {
    let bridge = ClaudeCodeBridge()
    bridge.setDeviceConnected(true)
    bridge.injectPermissionForTest(toolUseId: "t1", tool: "Bash", hint: "front")
    bridge.injectPermissionForTest(toolUseId: "t2", tool: "Write", hint: "behind")
    let behindId = bridge.queuedIdsForTest().last!
    // The device only ever shows the front, so a decision for a non-front id is illegitimate: reject it.
    XCTAssertEqual(bridge.resolve(id: behindId, approve: true), .unknown)
    XCTAssertEqual(bridge.queuedIdsForTest().count, 2, "rejected resolve must not advance the queue")
}

func testDrainDeniesEveryQueuedPrompt() {
    let bridge = ClaudeCodeBridge()
    bridge.setDeviceConnected(true)
    bridge.injectPermissionForTest(toolUseId: "t1", tool: "Bash", hint: "one")
    bridge.injectPermissionForTest(toolUseId: "t2", tool: "Write", hint: "two")
    let drained = expectation(description: "drained")
    bridge.drainHeldPrompts(reason: "quitting") { drained.fulfill() }
    wait(for: [drained], timeout: 1)
    XCTAssertTrue(bridge.queuedIdsForTest().isEmpty, "drain clears the whole queue")
}
```

Add the test accessors (the expire seam drives the hub-internal `finish(capped:)` path that the cap timer would fire — NOT the device `resolve` path):

```swift
func queuedIdsForTest() -> [String] { queue.sync { self.promptQueue } }
func expirePromptForTest(id: String) { queue.sync { _ = self.finish(id: id, approve: false, capped: true) } }
```

- [ ] **Step 2: Run to verify failure**

Run: `cd hub && swift test --filter testQueuedExpirySilently`
Expected: FAIL — `queuedIdsForTest` undefined.

- [ ] **Step 3: Silent queued-expiry (already holds) + front-only `resolve` guard**

A non-front `finish(capped:)` already removes only that id and calls `publishFront()` (Task 2), which leaves the front's id unchanged — so the device shows the same front with a decremented `qlen` and **no** "too late". Document the intent with a comment above `finish`:

```swift
// A non-front (queued) prompt resolving -- including its own 590s cap firing -- removes just that id
// and republishes the unchanged front with a lower qlen: the device never shows a "too late" for a
// prompt it never displayed (design decision 5, issue #98). The front keeps today's on-screen UX.
```

Then harden `resolve` (lines 176-178) so a device decision can only target the front (the only prompt it shows). Replace it with:

```swift
func resolve(id: String, approve: Bool) -> ResolveOutcome {
    queue.sync {
        // Device shows only the front; a decision for a live non-front id is out-of-order -> reject as
        // unknown (never advance the queue). A done id still falls through to finish -> .late tombstone.
        if let p = pending[id], !p.done, promptQueue.first != id { return .unknown }
        return finish(id: id, approve: approve, capped: false)
    }
}
```

- [ ] **Step 4: Extend `withdraw` to any queue position**

Replace `withdraw` (~389-400) so it removes from the FIFO and republishes the front (a withdraw can target a middle prompt now, not just the active one):

```swift
private func withdraw(id: String) {
    guard let p = pending[id], !p.done else { return }
    p.done = true
    p.resolvedAt = Date()
    p.timeout.cancel()
    promptQueue.removeAll { $0 == id }
    if !p.sessionId.isEmpty, let n = waitingSessionCounts[p.sessionId] {
        if n <= 1 { waitingSessionCounts.removeValue(forKey: p.sessionId) }
        else { waitingSessionCounts[p.sessionId] = n - 1 }
        buddy.waiting = waitingSessionCounts.count
    }
    publishFront()
    log(id: id, decision: "withdrawn-resolved-elsewhere")
}
```

Also extend the existing `reap(now:)` to drop resolved tombstones (replacing the prune we removed from `enqueuePrompt`). Add a constant near `sessionTTL`:

```swift
private static let tombstoneTTL: TimeInterval = 30   // keep a resolved id long enough for a racing late device decision -> .late
```

and inside `reap(now:)`, after the session pruning:

```swift
let tombCutoff = now.addingTimeInterval(-Self.tombstoneTTL)
for (pid, p) in pending where p.done && (p.resolvedAt ?? now) < tombCutoff {
    pending.removeValue(forKey: pid)   // bounded tombstone GC (replaces prune-on-enqueue)
}
```

Note: `applySessionHook`/`reap` currently recompute `buddy.waiting = waitingSessions.count`. Update **every** such reference to `waitingSessionCounts.count`, and any `waitingSessions.insert/remove(sid)` for session-level events (Stop/SessionEnd) to operate on the refcount map (drop the key entirely on SessionEnd; for Stop, clear that session's count). Grep `waitingSessions` across the file and convert all sites.

- [ ] **Step 5: Raise the hook timeout to the PermissionRequest max**

In `hub/claude-code-settings.snippet.json` line 9, change `"timeout": 190` to `"timeout": 600`. Update the `_comment` (line 2) phrase "PermissionRequest is 190 to cover the device's ~180s" to "PermissionRequest is 600 (its max) to cover the device's ~590s decision window". Grep the hub for other `190` occurrences and update each (`HooksDetectionTests.swift:14` expects 190 -> 600; check `HooksDetection.swift`/installer for a hardcoded `190`).

Run: `cd hub && rg -n '190' Sources Tests` -- confirm none remain that refer to the hook timeout.

- [ ] **Step 6: Run tests to verify they pass**

Run: `cd hub && swift test`
Expected: PASS (whole hub suite, incl. updated HooksDetection).

- [ ] **Step 7: Commit (USER runs)**

```bash
git add hub/Sources/beacon-hub/ClaudeCodeBridge.swift hub/claude-code-settings.snippet.json hub/Tests
git commit -m "feat(hub): hold queued prompts ~600s and drop expired queued prompts silently"
```

---

### Task 4: Firmware — parse `qlen` into the buddy record

Adds `queue_len` to the device prompt struct and fills it from the frame.

**Files:**
- Modify: `firmware/src/core/records.h:69-79` (`buddy_prompt_t`)
- Modify: `firmware/src/core/hub_proto.cpp:78-89` (prompt parse branch)
- Test: `firmware/test/test_hub_proto/` (existing suite)

**Interfaces:**
- Consumes: the `qlen` wire field (Task 1).
- Produces: `buddy_prompt_t.queue_len` (`uint8_t`, 1 when absent).

- [ ] **Step 1: Write the failing test**

In `firmware/test/test_hub_proto/`, add a case (match the file's existing Unity style):

```cpp
void test_parse_prompt_qlen(void) {
    const char* j = "{\"v\":1,\"buddy\":{\"prompt\":"
                    "{\"id\":\"p1\",\"tool\":\"Bash\",\"hint\":\"ls\",\"qlen\":3}}}";
    usage_rec_t u{}; buddy_rec_t b{}; bool hu=false, hb=false;
    TEST_ASSERT_TRUE(hub_parse_status(j, strlen(j), &u, &hu, &b, &hb));
    TEST_ASSERT_TRUE(b.prompt.present);
    TEST_ASSERT_EQUAL_UINT8(3, b.prompt.queue_len);
}

void test_parse_prompt_qlen_absent_defaults_one(void) {
    const char* j = "{\"v\":1,\"buddy\":{\"prompt\":"
                    "{\"id\":\"p1\",\"tool\":\"Bash\",\"hint\":\"ls\"}}}";
    usage_rec_t u{}; buddy_rec_t b{}; bool hu=false, hb=false;
    TEST_ASSERT_TRUE(hub_parse_status(j, strlen(j), &u, &hu, &b, &hb));
    TEST_ASSERT_EQUAL_UINT8(1, b.prompt.queue_len);
}

void test_parse_prompt_qlen_clamps_out_of_range(void) {
    usage_rec_t u{}; buddy_rec_t b{}; bool hu=false, hb=false;
    struct { const char* qlen; uint8_t want; } cases[] = {
        {"0", 1}, {"-3", 1}, {"256", 255}, {"1000", 255}, {"2", 2},
    };
    for (auto& c : cases) {
        char j[128];
        snprintf(j, sizeof(j),
            "{\"v\":1,\"buddy\":{\"prompt\":{\"id\":\"p1\",\"tool\":\"B\",\"hint\":\"h\",\"qlen\":%s}}}", c.qlen);
        TEST_ASSERT_TRUE(hub_parse_status(j, strlen(j), &u, &hu, &b, &hb));
        TEST_ASSERT_EQUAL_UINT8(c.want, b.prompt.queue_len);
    }
}
```

Register all three in the suite's `RUN_TEST` list.

- [ ] **Step 2: Run to verify failure**

Run: `~/.beacon-pio/bin/pio test -e native -f "*test_hub_proto*"`
Expected: FAIL — `queue_len` is not a member of `buddy_prompt_t`.

- [ ] **Step 3: Add the struct field**

In `firmware/src/core/records.h`, add to `buddy_prompt_t` (after `hint`, line 73):

```c
  uint8_t queue_len;           // total pending prompts incl. this front one (1 = lone); from qlen, NOT a local stamp
```

- [ ] **Step 4: Fill it in the parser**

In `firmware/src/core/hub_proto.cpp`, inside the `else` branch of the prompt parse (after line 88, `copy_trunc(... hint ...)`):

```cpp
      int q = p["qlen"] | 1;                                 // absent/non-numeric => 1
      buddy->prompt.queue_len = (uint8_t)(q < 1 ? 1 : (q > 255 ? 255 : q));  // clamp: 0/neg => 1, cap at 255
```

Read into an `int` first: a bare `(uint8_t)(p["qlen"] | 1)` would keep a literal `0`, wrap `256`->`0`, and mishandle negatives — none of which mean "single". The clamp normalizes all of those.

And in the `p.isNull()` idle branch (after line 77), reset it:

```cpp
      buddy->prompt.queue_len = 1;
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `~/.beacon-pio/bin/pio test -e native -f "*test_hub_proto*"`
Expected: PASS.

- [ ] **Step 6: Commit (USER runs)**

```bash
git add firmware/src/core/records.h firmware/src/core/hub_proto.cpp firmware/test/test_hub_proto
git commit -m "feat(firmware): parse qlen queue depth into the buddy prompt record"
```

---

### Task 5: Firmware — align device-local expiry to the ~600s hold

Raises the device's own prompt-expiry fail-safe so it does not flip the front prompt to `TOO_LATE` while the hub still holds it. (The `decision_state` reset on `prompt.id` change is already handled at `hub_proto.cpp:83-84`; this task adds a guard test for it.)

**Files:**
- Modify: `firmware/src/core/records.h:67` (`BUDDY_PROMPT_EXPIRY_S`)
- Test: `firmware/test/test_hub_proto/` (id-swap reset guard)

**Interfaces:** none new.

- [ ] **Step 1: Extend the existing reset test (do not add a duplicate)**

`firmware/test/test_hub_proto/test_main.cpp` already has `test_parse_new_prompt_resets_decision` covering the id-swap reset. Do NOT add a second test for it. Instead extend that existing test to also assert the queue advance carries `qlen` correctly — add these lines to the end of its body (a front advancing from a 2-deep queue to a lone next prompt):

```cpp
    // queue advance: front p1 (qlen 2) -> next p2 (lone) resets decision_state and drops the badge
    const char* nxt = "{\"v\":1,\"buddy\":{\"prompt\":{\"id\":\"p2\",\"tool\":\"Write\",\"hint\":\"b\"}}}";
    hub_parse_status(nxt, strlen(nxt), &u, &hu, &b, &hb);
    TEST_ASSERT_EQUAL_STRING("p2", b.prompt.id);
    TEST_ASSERT_EQUAL_UINT8(PROMPT_IDLE_DECISION, b.prompt.decision_state);
    TEST_ASSERT_EQUAL_UINT8(1, b.prompt.queue_len);
```

(Match the existing test's variable names — adjust `u`/`b`/`hu`/`hb` to whatever that test already declares.)

- [ ] **Step 2: Run to verify it passes already (guard) — but expiry constant unchanged**

Run: `~/.beacon-pio/bin/pio test -e native -f "*test_hub_proto*"`
Expected: PASS (the reset is pre-existing). This test locks the behavior so a future refactor cannot regress it.

- [ ] **Step 3: Raise the local expiry constant**

In `firmware/src/core/records.h` line 67, change:

```c
#define BUDDY_PROMPT_EXPIRY_S 590u   // align to the hub ~600s hold (CC PermissionRequest max); local fail-safe for a dropped hub link
```

- [ ] **Step 4: Run the full native suite**

Run: `~/.beacon-pio/bin/pio test -e native`
Expected: PASS. If a `test_datastore`/`test_buddy` case asserted the old 180s expiry, update it to 590s.

- [ ] **Step 5: Commit (USER runs)**

```bash
git add firmware/src/core/records.h firmware/test
git commit -m "feat(firmware): align buddy prompt local expiry to the ~600s hub hold"
```

---

### Task 6: Firmware — `(1 of N)` badge across the buddy views

Renders the queue depth in each view's default-state eyebrow when `queue_len > 1`.

**Files:**
- Modify: `firmware/src/ui/screens/screen_common.h` (add a shared `static inline` formatter — this repo has only the `.h`, no `screen_common.cpp`)
- Modify: `firmware/src/ui/screens/views/buddy_editorial.cpp:82-91` (worked example)
- Modify: the other 6 views: `buddy_hud.cpp`, `buddy_calm.cpp`, `buddy_blueprint.cpp`, `buddy_led.cpp`, `buddy_oscilloscope.cpp`, `buddy_analog.cpp`
- Test: `firmware/test/test_*` if a view-text helper test suite exists; otherwise the formatter gets a direct native test

**Interfaces:**
- Produces: `void buddy_queue_badge(uint8_t queue_len, char* out, size_t cap)` — writes `" (1 of N) "` when `queue_len > 1`, else `out[0]='\0'`.

- [ ] **Step 1: Write the failing test**

Add a small native suite `firmware/test/test_buddy_badge/test_buddy_badge.cpp`:

```cpp
#include <unity.h>
#include "ui/screens/screen_common.h"
#include <string.h>

void test_badge_hidden_when_single(void) {
    char out[24]; buddy_queue_badge(1, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}
void test_badge_shows_one_of_n(void) {
    char out[24]; buddy_queue_badge(3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(" (1 of 3)", out);
}
void setUp(void){} void tearDown(void){}
int main(){ UNITY_BEGIN(); RUN_TEST(test_badge_hidden_when_single); RUN_TEST(test_badge_shows_one_of_n); return UNITY_END(); }
```

- [ ] **Step 2: Run to verify failure**

Run: `~/.beacon-pio/bin/pio test -e native -f "*test_buddy_badge*"`
Expected: FAIL — `buddy_queue_badge` undefined.

- [ ] **Step 3: Add the shared formatter (header-only)**

This repo has only `screen_common.h` (no `.cpp`), and it already defines shared helpers `static inline` in the header (e.g. `static inline const char* fin_name(int i, const finance_rec_t& r)`, added in PR #102). Follow that exact precedent — add next to it, with `#include <stdio.h>` at the top if not already present (for `snprintf`):

```c
// Render the queue-position badge for the buddy prompt eyebrow: " (1 of N)" when queue_len>1, else "".
static inline void buddy_queue_badge(uint8_t queue_len, char* out, size_t cap) {
  if (queue_len > 1) snprintf(out, cap, " (1 of %u)", (unsigned)queue_len);
  else if (cap) out[0] = '\0';
}
```

- [ ] **Step 4: Wire it into the editorial view (worked example)**

In `buddy_editorial.cpp`, replace the default-case eyebrow block (lines 82-90):

```cpp
    default: {
      char badge[16]; buddy_queue_badge(b.prompt.queue_len, badge, sizeof(badge));
      char eb[48];
      snprintf(eb, sizeof(eb), "PERMISSION -- APPROVE?%s %us",
               badge, (unsigned)buddy_prompt_secs_left(&b, uptime_s()));
      txt_set(s_kicker, eb);
      txt_color(s_kicker, t->accent);
      txt_set(s_deny, "< DENY");
      txt_color(s_deny, t->ink_dim);
      txt_color(s_approve, t->accent);
      break;
    }
```

- [ ] **Step 5: Wire it into the other 6 views**

For each of `buddy_hud.cpp`, `buddy_calm.cpp`, `buddy_blueprint.cpp`, `buddy_led.cpp`, `buddy_oscilloscope.cpp`, `buddy_analog.cpp`: find the default-state eyebrow string (the one shown when `decision_state == PROMPT_IDLE_DECISION`, e.g. `"PERMISSION - APPROVE?"`). Build the badge and insert `%s` for it in that view's `snprintf`/`txt_set`, exactly as in Step 4 but keeping that view's own wording:

```cpp
char badge[16]; buddy_queue_badge(b.prompt.queue_len, badge, sizeof(badge));
// ... then concatenate `badge` into this view's existing eyebrow text (its wording, plus %s for badge)
```

Match each view's buffer sizing (bump a fixed `eb[]` to `eb[48]` if needed to fit ` (1 of N)`). Do not change any other text or layout.

- [ ] **Step 6: Run the formatter test + full build**

Run: `~/.beacon-pio/bin/pio test -e native -f "*test_buddy_badge*"`
Expected: PASS.
Run: `~/.beacon-pio/bin/pio run`
Expected: firmware compiles for ESP32-S3 (all 7 views build).

- [ ] **Step 7: Commit (USER runs)**

```bash
git add firmware/src/ui/screens firmware/test/test_buddy_badge
git commit -m "feat(firmware): show (1 of N) queue badge on the buddy prompt eyebrow"
```

---

### Task 7: Manual end-to-end verification

Proves the round-trip on real parts; no code, but the deliverable is evidence per CONTRIBUTING.md.

**Files:** none.

- [ ] **Step 1: Build + flash + run the hub**

Run: `cd firmware && ~/.beacon-pio/bin/pio run -t upload` and `cd hub && ./build-app.sh install-hooks && ./build-app.sh run`.

- [ ] **Step 2: Trigger concurrent prompts**

In two Claude Code sessions (two repos), each run a tool needing permission (e.g. a `Bash`/`Write`) at the same time.
Expected: device shows the first prompt with `(1 of 2)`; deciding it advances to the second showing it with no badge (1 left). Neither is auto-denied.

- [ ] **Step 3: Verify silent queued-expiry**

Queue two; leave both ~10 min so the behind one passes its 590s cap while the front waits.
Expected: the front is unchanged; the badge drops from `(1 of 2)` to no badge when the behind prompt expires; no "too late" flashes for the unseen prompt. Confirm the hub log shows `decision=deny-timeout` for the behind id.

- [ ] **Step 4: Record evidence**

Note board settings used; capture a photo/screen of the `(1 of N)` badge for the PR description.

---

## Self-Review Notes

- **Spec coverage:** decisions 1-6 map to Tasks 2 (queue/FIFO/advance), 1+4 (`qlen` wire), 3 (unbounded — busy-deny removed in Task 2 Step 5; 600s timeout; silent queued-expiry), 6 (`(1 of N)`); gotcha 1 -> Task 5 (expiry align), gotcha 2 -> Task 5 Step 1 (reset guard, already handled at `hub_proto.cpp:83-84`).
- **Back-compat:** `qlen` optional + omitted when `<=1` keeps single-prompt frames byte-identical, so existing CONTRACT/fixture tests do not churn.
- **Type consistency:** `BuddyPrompt(..., qlen: Int?)` (Task 1) consumed by `publishFront` (Task 2); `buddy_prompt_t.queue_len` (Task 4) consumed by `buddy_queue_badge` (Task 6).
- **Open verification during impl:** exact filenames of the 6 non-editorial buddy views and their eyebrow wording (Task 6 Step 5); presence of a hardcoded `190` in `HooksDetection.swift` vs only the snippet (Task 3 Step 5); whether `docs/tech.md` §7.1 inlines the prompt shape (Task 1 Step 5).
- **Codex review (2026-06-19) incorporated:** FIFO renamed `promptQueue` (no collision with the dispatch `queue`); `waitingSessions` -> refcount map (Task 2/3); front-only `resolve` guard + `expirePromptForTest` seam replacing the out-of-order `resolve`-as-expiry (Task 3); bounded tombstone reaper replacing prune-on-enqueue (Task 3); `qlen` parse clamp + boundary tests (Task 4); reset test extended not duplicated (Task 5); `buddy_queue_badge` `static inline` in the header, no phantom `.cpp` (Task 6); stale §C.3/§D + tech.md prose updates (Task 1/3).
