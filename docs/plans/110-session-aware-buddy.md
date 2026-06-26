# Session-Aware Coding Buddy — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the Coding Buddy from a permission gate into a glanceable, sound-cued dashboard of every Claude Code session's state, with one-tap focus (Phase 2) and optional device audio (Phase 3).

**Architecture:** The hub owns a per-session registry (state machine + opaque `s<n>` ids + branch lookup) and emits a **standalone `sessions` BLE frame**; the device holds a fixed-size session array and renders a list on the `claude` screen across all 7 themes. The existing `buddy` frame is unchanged (clean migration). Tap-to-open and device audio are spike-gated follow-ons.

**Tech Stack:** Swift 6 / SwiftPM (hub, `BeaconHubKit` pure logic + `beacon-hub` agent), C++ / PlatformIO / LVGL 8.4 (firmware), ArduinoJson (wire parse), Unity (firmware native tests), XCTest (hub tests).

**Design spec:** `docs/specs/2026-06-26-session-aware-buddy-design.md`. **Issue:** #110.

## Global Constraints

- **Toolchain pins (do not bump):** Arduino-ESP32 core 3.3.5, GFX 1.6.4, LVGL 8.4.0, core `BLE*` wrapper only. PlatformIO runs from `~/.beacon-pio` venv.
- **Native tests are pure C++** (Unity), no Arduino/LVGL; device-only code guarded `#if !BEACON_NATIVE`. Each `firmware/test/test_<unit>/` is one suite.
- **String fields are fixed-capacity NUL-terminated buffers; writers MUST truncate, never overflow** (`records.h` rule).
- **`HUB_FRAME_MAX` = 1024 B**; a frame over 1024 B is silently dropped by the device reassembler. Every emitted frame's worst case MUST be proven `< 1024`.
- **`sessions` is its OWN frame** `{"v":1,"sessions":[...]}` — never embed it in `buddy` (the combined `usage`+`buddy`+`loc` status frame already nears the cap).
- **Heap floor:** ≥60 KB free internal heap at all times (transient min ~53 KB). Phase 3 audio is gated on this.
- **BLE protocol is frozen in `hub/CONTRACT.md`** — `core/hub_proto.cpp`, `BeaconHubKit/Protocol.swift`, and CONTRACT.md stay in sync. Additive only; stays `v:1`.
- **Caps (frozen):** `sessions` ≤ 5; `label` ≤ 28 chars; `id` ≤ 6 chars; `state` ∈ {`working`,`waiting`,`waiting_queued`,`attention`,`idle`}.
- **Conventional Commits** (`type(scope): subject`, scopes `firmware`/`hub`/`docs`/`ci`). **The human operator performs every `git commit`** (project rule); each task's commit step stages files and supplies the message for the operator to run.
- **Privacy:** the hub never logs/transmits conversation content; session labels are folder + branch only.

---

## File Structure

**Hub — `BeaconHubKit` (pure, host-testable):**
- `Sources/BeaconHubKit/Protocol.swift` (modify) — add `SessionState`, `Session`, `SessionsFrame`, `SessionLimits`; add `DeviceCommand.open(id:)`.
- `Sources/BeaconHubKit/SessionRegistry.swift` (create) — per-session state machine, `s<n>` minter, snapshot/sort/cap, idle TTL. No Foundation Network — pure logic, `Date`-injectable.

**Hub — `beacon-hub` (agent):**
- `Sources/beacon-hub/ClaudeCodeBridge.swift` (modify) — feed the registry from existing hook handlers; add `onSessionsUpdate`; remove `Notification`→waiting; map front/queued prompt → session; async branch resolver.
- `Sources/beacon-hub/AppDelegate.swift` (modify) — wire `onSessionsUpdate` → BLE send; handle `DeviceCommand.open` (Phase 2); attention sound.
- `Sources/beacon-hub/MenubarController.swift` (modify) — `playAttentionSoundIfEnabled()` + debounce state.
- `Resources/beacon-attention.wav` (create) — the new "your turn" chime.

**Firmware — core:**
- `firmware/src/core/records.h` (modify) — `buddy_session_t`, caps, state enum, `buddy_rec_t.sessions[]`.
- `firmware/src/core/hub_proto.cpp` / `.h` (modify) — `hub_parse_sessions`; Phase 2 `hub_build_open`.
- `firmware/src/core/datastore.h` / `.cpp` (modify) — `ds_apply_sessions`; merge into the buddy record without clobbering scalars/prompt.
- `firmware/src/core/hub_task.cpp` (modify) — dispatch the `sessions` frame; Phase 2 `buddy_open` / `hub_send_open`.

**Firmware — UI (all 7 themes):**
- `firmware/src/ui/screens/views/view_common.h` (modify) — shared `buddy_session_split_label`, `buddy_session_age`, state-cue color/glyph helpers.
- `firmware/src/ui/screens/views/buddy_{calm,editorial,hud,led,analog,blueprint,oscilloscope}.cpp` (modify) — replace the idle-entries stack with a session list.

**Docs:**
- `hub/CONTRACT.md` (modify) — add the `sessions` frame + `open` command to §A/§B.
- `DESIGN.md` (modify) — session-row height note.
- `docs/tech.md`, `hub/CONTRACT.md` (modify) — reconcile the permission fail-closed timeout drift.
- `docs/spikes/2026-*-*.md` (create) — Phase 2 + Phase 3 spike write-ups.

---

# PHASE 1 — Session awareness (read-only)

### Task 1: `sessions` wire model + frame codec (Protocol.swift)

**Files:**
- Modify: `hub/Sources/BeaconHubKit/Protocol.swift`
- Test: `hub/Tests/BeaconHubKitTests/SessionsFrameTests.swift` (create)

**Interfaces:**
- Produces: `enum SessionState: String, Codable` (`working`,`waiting`,`waitingQueued="waiting_queued"`,`attention`,`idle`); `struct Session: Codable, Equatable {var id; var label; var state: SessionState; var ts: Int}`; `struct SessionsFrame {init(_ sessions:[Session]); func encoded() throws -> Data}`; `enum SessionLimits {maxCount=5; labelMaxChars=28; idMaxChars=6}`.

- [ ] **Step 1: Write the failing test**

```swift
import XCTest
@testable import BeaconHubKit

final class SessionsFrameTests: XCTestCase {
    func testEncodesWireShape() throws {
        let f = SessionsFrame([
            Session(id: "s3", label: "beacon · fix/109", state: .attention, ts: 1719400000),
            Session(id: "s1", label: "api · main", state: .working, ts: 1719399860),
        ])
        let s = String(decoding: try f.encoded(), as: UTF8.self)
        XCTAssertTrue(s.hasSuffix("\n"))
        XCTAssertTrue(s.contains("\"v\":1"))
        XCTAssertTrue(s.contains("\"state\":\"attention\""))
        XCTAssertTrue(s.contains("\"state\":\"working\""))
    }

    // Worst case MUST stay < HUB_FRAME_MAX (1024). 5 rows, max-length id + label.
    func testWorstCaseUnderFrameMax() throws {
        let rows = (0..<SessionLimits.maxCount).map { _ in
            Session(id: "s99999", label: String(repeating: "W", count: SessionLimits.labelMaxChars),
                    state: .waitingQueued, ts: 1719400000)
        }
        let bytes = try SessionsFrame(rows).encoded().count
        XCTAssertLessThan(bytes, 1024, "sessions frame worst case must fit HUB_FRAME_MAX; got \(bytes)")
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd hub && swift test --filter SessionsFrameTests`
Expected: FAIL — `SessionState`/`Session`/`SessionsFrame`/`SessionLimits` undefined.

- [ ] **Step 3: Add the model + codec**

In `Protocol.swift`, after the `Loc` struct, add:

```swift
public enum SessionLimits { public static let maxCount = 5; public static let labelMaxChars = 28; public static let idMaxChars = 6 }

public enum SessionState: String, Codable, Equatable {
    case working, waiting, attention, idle
    case waitingQueued = "waiting_queued"
}

public struct Session: Codable, Equatable {
    public var id: String
    public var label: String
    public var state: SessionState
    public var ts: Int            // Unix epoch seconds of last update
    public init(id: String, label: String, state: SessionState, ts: Int) {
        self.id = id; self.label = label; self.state = state; self.ts = ts
    }
}

// Standalone hub->device frame (design §4). NOT embedded in `buddy`: the combined status frame
// (usage+buddy+loc) already nears HUB_FRAME_MAX; a separate frame keeps the budget independent and
// lets old firmware ignore it (it still reads the unchanged `buddy`/`entries` frame).
public struct SessionsFrame: Codable {
    public var sessions: [Session]
    public let v: Int
    // Defensive cap/truncation at the wire boundary even though SessionRegistry is the sole producer:
    // guarantees the frame can never exceed the frozen caps regardless of caller.
    public init(_ sessions: [Session]) {
        self.sessions = sessions.prefix(SessionLimits.maxCount).map {
            Session(id: String($0.id.prefix(SessionLimits.idMaxChars)),
                    label: String($0.label.prefix(SessionLimits.labelMaxChars)),
                    state: $0.state, ts: $0.ts)
        }
        self.v = 1
    }
    public func encoded() throws -> Data {
        let enc = JSONEncoder(); enc.outputFormatting = [.sortedKeys]
        var d = try enc.encode(self); d.append(0x0A); return d
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd hub && swift test --filter SessionsFrameTests`
Expected: PASS (both tests).

- [ ] **Step 5: Commit**

```bash
git add hub/Sources/BeaconHubKit/Protocol.swift hub/Tests/BeaconHubKitTests/SessionsFrameTests.swift
git commit -m "feat(hub): add sessions wire model + standalone frame codec"
```

---

### Task 2: `SessionRegistry` — state machine, minter, snapshot (pure)

**Files:**
- Create: `hub/Sources/BeaconHubKit/SessionRegistry.swift`
- Test: `hub/Tests/BeaconHubKitTests/SessionRegistryTests.swift` (create)

**Interfaces:**
- Consumes: `Session`, `SessionState`, `SessionLimits` (Task 1).
- Produces:
  - `final class SessionRegistry` with `init(idleTTL: TimeInterval = 300)`.
  - `func touchActivity(sessionId: String, cwd: String?, now: Date)` — establishes/keeps a session, state→working.
  - `func markStop(sessionId: String, now: Date)` — state→attention (until next activity).
  - `func setBranch(sessionId: String, branch: String?)`.
  - `func end(sessionId: String)` — remove.
  - `func reap(now: Date)` — drop sessions silent > a removal TTL (reuse 600 s).
  - `func snapshot(now: Date, waitingFront: String?, waitingQueued: Set<String>) -> [Session]` — applies precedence, sorts by updatedAt desc, caps to `SessionLimits.maxCount`, builds + truncates the label.

- [ ] **Step 1: Write the failing tests (table-driven precedence + sort/cap)**

```swift
import XCTest
@testable import BeaconHubKit

final class SessionRegistryTests: XCTestCase {
    private let t0 = Date(timeIntervalSince1970: 1_719_400_000)

    func testStatePrecedence() {
        struct C { let name: String; let setup: (SessionRegistry) -> Void
                   let front: String?; let queued: Set<String>; let expect: SessionState }
        let cases: [C] = [
            C(name: "activity => working", setup: { $0.touchActivity(sessionId: "A", cwd: "/x/api", now: self.t0) },
              front: nil, queued: [], expect: .working),
            C(name: "stop => attention", setup: {
                $0.touchActivity(sessionId: "A", cwd: "/x/api", now: self.t0)
                $0.markStop(sessionId: "A", now: self.t0.addingTimeInterval(1)) },
              front: nil, queued: [], expect: .attention),
            C(name: "front prompt overrides attention", setup: {
                $0.touchActivity(sessionId: "A", cwd: "/x/api", now: self.t0)
                $0.markStop(sessionId: "A", now: self.t0) },
              front: "A", queued: [], expect: .waiting),
            C(name: "queued prompt", setup: { $0.touchActivity(sessionId: "A", cwd: "/x/api", now: self.t0) },
              front: "B", queued: ["A"], expect: .waitingQueued),
            C(name: "stale => idle", setup: { $0.touchActivity(sessionId: "A", cwd: "/x/api", now: self.t0) },
              front: nil, queued: [], expect: .idle),  // queried 301s later below
            C(name: "stale stopped => idle (B3)", setup: {     // a long-ago Stop must NOT stay attention
                $0.touchActivity(sessionId: "A", cwd: "/x/api", now: self.t0)
                $0.markStop(sessionId: "A", now: self.t0) },
              front: nil, queued: [], expect: .idle),
        ]
        for c in cases {
            let r = SessionRegistry(idleTTL: 300)
            c.setup(r)
            let now = c.name.contains("idle") ? t0.addingTimeInterval(301) : t0.addingTimeInterval(2)
            let snap = r.snapshot(now: now, waitingFront: c.front, waitingQueued: c.queued)
            XCTAssertEqual(snap.first?.state, c.expect, c.name)
        }
    }

    func testActivityClearsAttention() {
        let r = SessionRegistry()
        r.touchActivity(sessionId: "A", cwd: "/x/api", now: t0)
        r.markStop(sessionId: "A", now: t0.addingTimeInterval(1))
        r.touchActivity(sessionId: "A", cwd: "/x/api", now: t0.addingTimeInterval(2))   // resumed
        let snap = r.snapshot(now: t0.addingTimeInterval(3), waitingFront: nil, waitingQueued: [])
        XCTAssertEqual(snap.first?.state, .working)
    }

    func testLabelAndIdMintingAndCap() {
        let r = SessionRegistry()
        for i in 0..<8 { r.touchActivity(sessionId: "S\(i)", cwd: "/x/repo\(i)", now: t0.addingTimeInterval(Double(i))) }
        let snap = r.snapshot(now: t0.addingTimeInterval(10), waitingFront: nil, waitingQueued: [])
        XCTAssertEqual(snap.count, SessionLimits.maxCount)                 // capped at 5
        XCTAssertEqual(snap.first?.id.first, "s")                          // s<n>
        XCTAssertLessThanOrEqual(snap.first!.id.count, SessionLimits.idMaxChars)
        XCTAssertTrue(snap.map(\.ts) == snap.map(\.ts).sorted(by: >))      // sorted by ts desc
    }

    func testBranchInLabelAndTruncation() {
        let r = SessionRegistry()
        r.touchActivity(sessionId: "A", cwd: "/home/user/beacon", now: t0)
        r.setBranch(sessionId: "A", branch: "fix/109-buddy")
        let snap = r.snapshot(now: t0.addingTimeInterval(1), waitingFront: nil, waitingQueued: [])
        XCTAssertEqual(snap.first?.label, "beacon · fix/109-buddy")
        XCTAssertLessThanOrEqual(snap.first!.label.count, SessionLimits.labelMaxChars)
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd hub && swift test --filter SessionRegistryTests`
Expected: FAIL — `SessionRegistry` undefined.

- [ ] **Step 3: Implement the registry**

```swift
import Foundation

// Pure per-session state machine (design §2/§3). Date-injected so it is unit-testable without wall time.
// The bridge feeds it from CC hooks; snapshot() renders the wire `[Session]`. Mints opaque monotonic
// `s<n>` ids (never reused in a process lifetime), distinct from the `p<n>` prompt namespace.
public final class SessionRegistry {
    private struct Entry {
        let shortId: String
        var cwd: String
        var branch: String?
        var lastActivity: Date
        var updatedAt: Date     // max(lastActivity, stoppedAt) — the sort key
        var stopped: Bool       // last lifecycle event was Stop, no newer activity => attention
    }
    private var entries: [String: Entry] = [:]    // keyed by CC session_id
    private var counter: UInt32 = 0
    private let idleTTL: TimeInterval
    private static let removeTTL: TimeInterval = 600

    public init(idleTTL: TimeInterval = 300) { self.idleTTL = idleTTL }

    // Opaque, monotonic, never reused for a live session. Wraps modulo 100000 to honor the frozen
    // 6-char id cap ("s" + <=5 digits); collisions are impossible in practice — reaching 100k sessions
    // in one hub process would require the first to be long gone (removeTTL = 600 s).
    private func mintId() -> String { counter = (counter &+ 1) % 100000; return "s\(counter)" }

    public func touchActivity(sessionId: String, cwd: String?, now: Date) {
        guard !sessionId.isEmpty else { return }
        if var e = entries[sessionId] {
            if let cwd, !cwd.isEmpty { e.cwd = cwd }
            e.lastActivity = now; e.updatedAt = now; e.stopped = false
            entries[sessionId] = e
        } else {
            entries[sessionId] = Entry(shortId: mintId(), cwd: cwd ?? "", branch: nil,
                                       lastActivity: now, updatedAt: now, stopped: false)
        }
    }

    public func markStop(sessionId: String, now: Date) {
        guard var e = entries[sessionId] else { return }
        e.stopped = true; e.updatedAt = now; entries[sessionId] = e
    }

    public func setBranch(sessionId: String, branch: String?) {
        guard var e = entries[sessionId] else { return }
        e.branch = (branch?.isEmpty == true) ? nil : branch; entries[sessionId] = e
    }

    public func end(sessionId: String) { entries.removeValue(forKey: sessionId) }

    public func reap(now: Date) {
        let cutoff = now.addingTimeInterval(-Self.removeTTL)
        for (sid, e) in entries where e.updatedAt < cutoff { entries.removeValue(forKey: sid) }
    }

    public func snapshot(now: Date, waitingFront: String?, waitingQueued: Set<String>) -> [Session] {
        entries.map { (sid, e) -> (Date, Session) in
            let state: SessionState
            let stale = now.timeIntervalSince(e.updatedAt) >= idleTTL
            if sid == waitingFront { state = .waiting }
            else if waitingQueued.contains(sid) { state = .waitingQueued }
            else if stale { state = .idle }                 // idle TTL wins over a long-ago Stop (Codex B3)
            else if e.stopped { state = .attention }
            else { state = .working }
            return (e.updatedAt, Session(id: e.shortId, label: Self.label(e.cwd, e.branch),
                                         state: state, ts: Int(e.updatedAt.timeIntervalSince1970)))
        }
        .sorted { $0.0 > $1.0 }
        .prefix(SessionLimits.maxCount)
        .map { $0.1 }
    }

    // Default branches add no information, so they are dropped ("redundant", spec §4). The device
    // splits the remaining "folder · branch" for its two-line row (Task 10).
    private static let redundantBranches: Set<String> = ["main", "master"]
    static func label(_ cwd: String, _ branch: String?) -> String {
        let folder = cwd.split(separator: "/").last.map(String.init) ?? cwd
        let showBranch = branch.map { !$0.isEmpty && !redundantBranches.contains($0) } ?? false
        let full = showBranch ? "\(folder) · \(branch!)" : folder
        return full.count <= SessionLimits.labelMaxChars
            ? full : String(full.prefix(SessionLimits.labelMaxChars))
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd hub && swift test --filter SessionRegistryTests`
Expected: PASS (all four tests).

- [ ] **Step 5: Commit**

```bash
git add hub/Sources/BeaconHubKit/SessionRegistry.swift hub/Tests/BeaconHubKitTests/SessionRegistryTests.swift
git commit -m "feat(hub): add pure SessionRegistry state machine"
```

---

### Task 3: Wire the registry into `ClaudeCodeBridge` + emit `onSessionsUpdate`

**Files:**
- Modify: `hub/Sources/beacon-hub/ClaudeCodeBridge.swift`
- Test: `hub/Tests/beacon-hubTests/BridgeSessionsTests.swift` (create or extend existing bridge tests)

**Interfaces:**
- Consumes: `SessionRegistry`, `Session` (Tasks 1-2).
- Produces: `var onSessionsUpdate: (([Session]) -> Void)?` on `ClaudeCodeBridge`; sessions published on every buddy-affecting event.

- [ ] **Step 1: Write the failing test**

Drive the existing test seams (the bridge already exposes `injectPermissionForTest`, `reap(now:)`, `handleStatusline`). Add a sessions sink and assert state transitions.

```swift
import XCTest
import BeaconHubKit
@testable import beacon_hub   // module name per Package.swift

final class BridgeSessionsTests: XCTestCase {
    // onSessionsUpdate hops to DispatchQueue.main.async (like onBuddyUpdate); pump main before asserting.
    private func drainMain() {
        let e = expectation(description: "main"); DispatchQueue.main.async { e.fulfill() }
        wait(for: [e], timeout: 1)
    }

    func testStopProducesAttentionThenActivityClears() {
        let b = ClaudeCodeBridge()
        var last: [Session] = []
        b.onSessionsUpdate = { last = $0 }
        b.applySessionHookForTest(event: "SessionStart", sessionId: "A", cwd: "/x/api")
        b.applySessionHookForTest(event: "Stop", sessionId: "A", cwd: "/x/api")
        drainMain()
        XCTAssertEqual(last.first(where: { $0.label.hasPrefix("api") })?.state, .attention)
        b.handleStatusline(["session_id": "A", "context_window": ["used_percentage": 10]])   // already internal
        drainMain()
        XCTAssertEqual(last.first(where: { $0.label.hasPrefix("api") })?.state, .working)
    }

    func testNotificationNoLongerForcesWaiting() {
        let b = ClaudeCodeBridge()
        var last: [Session] = []
        b.onSessionsUpdate = { last = $0 }
        b.applySessionHookForTest(event: "SessionStart", sessionId: "A", cwd: "/x/api")
        b.applySessionHookForTest(event: "Notification", sessionId: "A", cwd: "/x/api")
        drainMain()
        XCTAssertNotEqual(last.first?.state, .waiting)   // Notification is not a held prompt
    }

    func testPermissionFirstSignalAppearsAsWaiting() {   // Codex B2: permission must register the session
        let b = ClaudeCodeBridge()
        var last: [Session] = []
        b.onSessionsUpdate = { last = $0 }
        b.injectPermissionForTest(toolUseId: "P", tool: "Bash", hint: "x")  // session "P", no prior hook
        drainMain()
        XCTAssertEqual(last.first(where: { $0.id.hasPrefix("s") })?.state, .waiting)
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd hub && swift test --filter BridgeSessionsTests`
Expected: FAIL — `onSessionsUpdate` / `applySessionHookForTest` undefined.

- [ ] **Step 3: Implement the wiring**

In `ClaudeCodeBridge.swift`:

1. Add the property near the other callbacks (after line 24):
```swift
    var onSessionsUpdate: (([Session]) -> Void)?
```
2. Add the registry instance near `buddy` (after line 46):
```swift
    private let registry = SessionRegistry(idleTTL: 300)
    private var lastPublishedSessions: [Session] = []
```
3. In `applySessionHook(event:body:)` (line 455), after `touch(sid)`, feed the registry; **remove** the `Notification`→waiting block (lines 462-470), replacing the whole `switch` body:
```swift
        let cwd = body["cwd"] as? String
        switch event {
        case "Stop":
            if let sid { waitingSessionCounts.removeValue(forKey: sid); registry.markStop(sessionId: sid, now: Date()) }
        case "Notification":
            break   // Notification is NOT a held prompt; waiting derives only from enqueuePrompt (design §2, Codex #2).
        case "SessionEnd":
            if let sid {
                sessions.removeValue(forKey: sid); waitingSessionCounts.removeValue(forKey: sid)
                sessionStats.removeValue(forKey: sid); registry.end(sessionId: sid)
            }
        default:   // SessionStart + any: activity establishes the session as working.
            if let sid { registry.touchActivity(sessionId: sid, cwd: cwd, now: Date()) }
        }
```
   Note: `Stop` must also keep the session known to the registry, so call `registry.touchActivity` first when the entry may not exist — guard inside `markStop` already no-ops a missing entry, so add a touch on SessionStart/activity paths only (statusline + default). For `Stop` on an unseen session, prepend:
```swift
        if let sid, event == "Stop" { registry.touchActivity(sessionId: sid, cwd: cwd, now: Date()) ; registry.markStop(sessionId: sid, now: Date()) }
```
   (Fold this into the `Stop` case; drop the duplicate.)
4. In `handleStatusline(_:)` (line 494), after `touch(sid)`, add:
```swift
        if let sid { registry.touchActivity(sessionId: sid, cwd: body["cwd"] as? String, now: Date()) }
```
5. Add session publishing helper + map the prompt queue to front/queued session ids:
```swift
    private func waitingSessionSets() -> (front: String?, queued: Set<String>) {
        guard let frontId = promptQueue.first, let fp = pending[frontId], !fp.done else { return (nil, []) }
        let front = fp.sessionId.isEmpty ? nil : fp.sessionId
        var queued = Set<String>()
        for id in promptQueue.dropFirst() {
            if let p = pending[id], !p.done, !p.sessionId.isEmpty { queued.insert(p.sessionId) }
        }
        queued.subtract(front.map { [$0] } ?? [])
        return (front, queued)
    }

    private func publishSessions() {
        registry.reap(now: Date())
        let (front, queued) = waitingSessionSets()
        let snap = registry.snapshot(now: Date(), waitingFront: front, waitingQueued: queued)
        guard snap != lastPublishedSessions else { return }
        lastPublishedSessions = snap
        let cb = onSessionsUpdate
        DispatchQueue.main.async { cb?(snap) }
    }
```
6. In `enqueuePrompt` (line 354), register the prompt's session so a permission can be the FIRST signal for a session (Codex B2) — add right after the `waitingSessionCounts` bump:
```swift
        if !sessionId.isEmpty { registry.touchActivity(sessionId: sessionId, cwd: nil, now: Date()) }
```
7. Call `publishSessions()` at the end of `applySessionHook`, `handleStatusline`, `enqueuePrompt`, `finish`, `withdraw`, and `reap(now:)` (every place that already calls `publishBuddy()`/`publishFront()`).
8. Add the test seam near `injectPermissionForTest`:
```swift
    func applySessionHookForTest(event: String, sessionId: String, cwd: String) {
        queue.sync { self.applySessionHook(event: event, body: ["session_id": sessionId, "cwd": cwd, "hook_event_name": event]) }
    }
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd hub && swift test --filter BridgeSessionsTests && swift test`
Expected: PASS, and no regression in existing bridge tests (waiting/running counts still derive from `waitingSessionCounts`/`sessions`).

- [ ] **Step 5: Commit**

```bash
git add hub/Sources/beacon-hub/ClaudeCodeBridge.swift hub/Tests/beacon-hubTests/BridgeSessionsTests.swift
git commit -m "feat(hub): feed SessionRegistry from CC hooks; drop Notification=>waiting"
```

---

### Task 4: Async branch resolver (off the bridge serial queue)

**Files:**
- Modify: `hub/Sources/beacon-hub/ClaudeCodeBridge.swift`
- Test: `hub/Tests/beacon-hubTests/BridgeSessionsTests.swift` (extend)

**Interfaces:**
- Produces: branch is resolved off-queue and cached per `cwd`; `setBranch` lands on the registry; never blocks a hook reply.

- [ ] **Step 1: Write the failing test (cache seam)**

```swift
    func testBranchResolvedAndCachedPerCwd() {
        let b = ClaudeCodeBridge()
        var resolves = 0
        // Use a NON-default branch: SessionRegistry.label() drops main/master, so a default branch
        // would never appear in the label and couldn't be asserted.
        b.branchResolverForTest = { _ in resolves += 1; return "feat/x" }
        var last: [Session] = []
        b.onSessionsUpdate = { last = $0 }
        b.applySessionHookForTest(event: "SessionStart", sessionId: "A", cwd: "/x/api")
        b.applySessionHookForTest(event: "Stop", sessionId: "B", cwd: "/x/api")  // same cwd
        let exp = expectation(description: "branch")
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { exp.fulfill() }
        wait(for: [exp], timeout: 1)
        XCTAssertEqual(resolves, 1, "same cwd resolves git once")
        XCTAssertTrue(last.contains { $0.label.contains("· feat/x") })
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd hub && swift test --filter BridgeSessionsTests/testBranchResolvedAndCachedPerCwd`
Expected: FAIL — `branchResolverForTest` undefined.

- [ ] **Step 3: Implement the resolver**

```swift
    // Injectable for tests; production runs `git -C <cwd> rev-parse --abbrev-ref HEAD` off-queue.
    var branchResolverForTest: ((String) -> String?)?
    private var branchCache: [String: String] = [:]            // cwd => resolved branch (queue-confined)
    private var branchInFlight: [String: [String]] = [:]       // cwd => sessionIds awaiting one in-flight resolve
    private let gitQueue = DispatchQueue(label: "beacon.git", qos: .utility)

    // Call from applySessionHook/handleStatusline AFTER registry.touchActivity, on `queue`. Dedupes by
    // cwd: a cached branch applies immediately; a cwd already resolving just appends the session to the
    // waiter list (Codex B4 — never spawns a second git for the same cwd).
    private func ensureBranch(sessionId: String, cwd: String?) {
        guard let cwd, !cwd.isEmpty else { return }
        if let cached = branchCache[cwd] { registry.setBranch(sessionId: sessionId, branch: cached); return }
        if branchInFlight[cwd] != nil { branchInFlight[cwd]!.append(sessionId); return }   // already resolving
        branchInFlight[cwd] = [sessionId]
        let resolver = branchResolverForTest
        gitQueue.async { [weak self] in
            let branch = resolver?(cwd) ?? Self.gitBranch(cwd)
            self?.queue.async {
                guard let self else { return }
                if let branch, !branch.isEmpty { self.branchCache[cwd] = branch }
                for sid in self.branchInFlight[cwd] ?? [] { self.registry.setBranch(sessionId: sid, branch: branch) }
                self.branchInFlight.removeValue(forKey: cwd)
                self.publishSessions()
            }
        }
    }

    private static func gitBranch(_ cwd: String) -> String? {
        let p = Process(); p.executableURL = URL(fileURLWithPath: "/usr/bin/git")
        p.arguments = ["-C", cwd, "rev-parse", "--abbrev-ref", "HEAD"]
        let pipe = Pipe(); p.standardOutput = pipe; p.standardError = Pipe()
        do { try p.run() } catch { return nil }
        p.waitUntilExit()
        guard p.terminationStatus == 0 else { return nil }
        let out = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        let b = out.trimmingCharacters(in: .whitespacesAndNewlines)
        return (b.isEmpty || b == "HEAD") ? nil : b
    }
```
Call `ensureBranch(sessionId: sid, cwd: cwd)` in the `default`/`Stop`/statusline activity paths (where `registry.touchActivity` is called).

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd hub && swift test --filter BridgeSessionsTests`
Expected: PASS. Confirm the hook-reply path is never blocked (git runs on `gitQueue`, result hops back to `queue`).

- [ ] **Step 5: Commit**

```bash
git add hub/Sources/beacon-hub/ClaudeCodeBridge.swift hub/Tests/beacon-hubTests/BridgeSessionsTests.swift
git commit -m "feat(hub): resolve git branch async, cached per cwd"
```

---

### Task 5: Send the `sessions` frame over BLE (AppDelegate)

**Files:**
- Modify: `hub/Sources/beacon-hub/AppDelegate.swift`

**Interfaces:**
- Consumes: `onSessionsUpdate` (Task 3), `SessionsFrame.encoded()` (Task 1), the existing BLE central write path used for status frames.

- [ ] **Step 1: Wire the callback (no unit test — integration; verified at Step 2 via build + manual)**

`AppDelegate` is `@MainActor` and every bridge callback hops via `Task { @MainActor in … }` (e.g. `onBuddyUpdate` at line 168). Mirror that exactly.

1. Add a cached snapshot next to `private var buddy = BuddyState()` (line 26), so the reconnect path can resend it (Codex B6):
```swift
    private var sessions: [Session] = []
```
2. Wire the callback alongside `bridge.onBuddyUpdate` (line 168):
```swift
        bridge.onSessionsUpdate = { [weak self] sessions in
            Task { @MainActor in
                self?.sessions = sessions
                if let data = try? SessionsFrame(sessions).encoded() { self?.central.send(data) }
            }
        }
```
3. In `central.onReady` (line 196), right after `sendFullFrame(includeLocation: true)` (line 203), resend the current list to the freshly-(re)subscribed device:
```swift
                if let data = try? SessionsFrame(self?.sessions ?? []).encoded() { self?.central.send(data) }
```

- [ ] **Step 2: Build + verify**

Run: `cd hub && swift build`
Expected: builds clean. Manual: with a paired device, start a Claude session → device receives a sessions frame (verify via serial log on the device once Task 9 lands; until then, confirm the bytes are written via a hub debug log).

- [ ] **Step 3: Commit**

```bash
git add hub/Sources/beacon-hub/AppDelegate.swift
git commit -m "feat(hub): transmit sessions frame to device on update + reconnect"
```

---

### Task 6: New `beacon-attention.wav` + debounced attention sound

**Files:**
- Create: `hub/Resources/beacon-attention.wav`
- Modify: `hub/Sources/beacon-hub/MenubarController.swift`, `hub/Sources/beacon-hub/AppDelegate.swift`, `hub/Sources/beacon-hub/ClaudeCodeBridge.swift`

**Interfaces:**
- Produces: `var onAttention: (() -> Void)?` on the bridge (fires on a `0→>0` attention-bucket transition, 1.5 s same-session suppression); `MenubarController.playAttentionSoundIfEnabled()`.

- [ ] **Step 1: Add the sound asset + bundle it**

Add a short (~0.4 s) distinct chime at `hub/Resources/beacon-attention.wav` (clearly different timbre from `beacon-prompt.wav`). It is NOT auto-bundled — `build-app.sh` copies resources explicitly (line 115). Add beside the prompt-wav copy:
```bash
cp Resources/beacon-attention.wav "$APP/Contents/Resources/"   # attention chime (your-turn)
```

- [ ] **Step 2: Write the failing test (aggregate 0->>0 debounce)**

Add to `BridgeSessionsTests` (it has `drainMain()`):
```swift
    func testAttentionFiresOncePerBucketTransition() {
        let b = ClaudeCodeBridge()
        var fires = 0
        b.onAttention = { fires += 1 }
        b.applySessionHookForTest(event: "SessionStart", sessionId: "A", cwd: "/x/api")
        b.applySessionHookForTest(event: "SessionStart", sessionId: "B", cwd: "/x/api")
        b.applySessionHookForTest(event: "Stop", sessionId: "A", cwd: "/x/api")   // 0 -> >0 attention: fire
        b.applySessionHookForTest(event: "Stop", sessionId: "B", cwd: "/x/api")   // bucket already >0: no fire
        drainMain()
        XCTAssertEqual(fires, 1)   // aggregate per-bucket, not per-session (spec §6)
    }
```

- [ ] **Step 3: Implement aggregate transition detection in the bridge**

Add `var onAttention: (() -> Void)?`. In `publishSessions()`, BEFORE assigning `lastPublishedSessions = snap`, compare the attention bucket against the previous snapshot — fire only on the aggregate empty→non-empty edge (spec §6: "chime on the aggregate 0→>0 transition per state bucket"):
```swift
        let prevAttention = lastPublishedSessions.contains { $0.state == .attention }
        let nowAttention  = snap.contains { $0.state == .attention }
        if !prevAttention && nowAttention {     // bucket went 0 -> >0
            let cb = onAttention
            DispatchQueue.main.async { cb?() }
        }
```
A session re-entering attention while another is already in the bucket does not re-chime (the bucket never returned to 0), satisfying the same-session suppression without a separate timer.

- [ ] **Step 4: Wire the sound (MenubarController + AppDelegate)**

In `MenubarController.swift`, mirror `playPromptSoundIfEnabled()`:
```swift
    private let attentionSound: NSSound? = {
        if let url = Bundle.main.url(forResource: "beacon-attention", withExtension: "wav") {
            return NSSound(contentsOf: url, byReference: true)
        }
        return NSSound(named: "Submarine")
    }()
    func playAttentionSoundIfEnabled() { guard !promptSoundMuted else { return }; attentionSound?.stop(); attentionSound?.play() }
```
In `AppDelegate`, alongside `bridge.onPromptArrived` (line 181), wrapped like every other callback (`@MainActor`):
```swift
        bridge.onAttention = { [weak self] in
            Task { @MainActor in self?.menubar.playAttentionSoundIfEnabled() }
        }
```

- [ ] **Step 5: Run tests + build**

Run: `cd hub && swift test --filter BridgeSessionsTests && swift build`
Expected: PASS + clean build. Manual: a session finishing its turn chimes `beacon-attention.wav` once; a held permission still chimes `beacon-prompt.wav`.

- [ ] **Step 6: Commit**

```bash
git add hub/Resources/beacon-attention.wav hub/Sources/beacon-hub/MenubarController.swift hub/Sources/beacon-hub/AppDelegate.swift hub/Sources/beacon-hub/ClaudeCodeBridge.swift hub/Tests/beacon-hubTests/BridgeSessionsTests.swift
git commit -m "feat(hub): debounced attention chime on Stop transition"
```

---

### Task 7: Firmware record schema — `buddy_session_t`

**Files:**
- Modify: `firmware/src/core/records.h`

**Interfaces:**
- Produces: `BUDDY_SID_LEN`, `BUDDY_LABEL_LEN`, `BUDDY_SESSIONS_MAX`, the `BST_*` enum, `buddy_session_t`, and `buddy_rec_t.sessions[]` + `session_count`.

- [ ] **Step 1: Add the schema (no isolated test; consumed by Task 8's parser tests)**

After the `BUDDY_ENTRIES` defines (line 13) add:
```c
#define BUDDY_SID_LEN      8   // "s" + up to 6 digits + NUL
#define BUDDY_LABEL_LEN   29   // 28 chars + NUL (design §4 cap)
#define BUDDY_SESSIONS_MAX 5

enum {                          // wire `state` string => firmware enum
  BST_WORKING = 0,
  BST_WAITING,
  BST_WAITING_QUEUED,
  BST_ATTENTION,
  BST_IDLE,
};
typedef struct {
  char     id[BUDDY_SID_LEN];   // opaque hub-minted s<n>, echoed back on tap (Phase 2)
  char     label[BUDDY_LABEL_LEN];
  uint8_t  state;               // BST_*
  uint32_t ts;                  // epoch seconds of last update (sort key, age source)
} buddy_session_t;
```
In `buddy_rec_t` (line 83), after `buddy_prompt_t prompt;` add:
```c
  buddy_session_t sessions[BUDDY_SESSIONS_MAX];  // newest-first (hub-sorted); arrives in its OWN frame
  uint8_t         session_count;
```

- [ ] **Step 2: Build the native test env to confirm it compiles**

Run: `cd firmware && ~/.beacon-pio/bin/pio test -e native -f "*test_hub_proto*"`
Expected: compiles (tests still pass; no behavior change yet).

- [ ] **Step 3: Commit**

```bash
git add firmware/src/core/records.h
git commit -m "feat(firmware): add buddy_session_t record schema"
```

---

### Task 8: Firmware `sessions` frame parser + native tests

**Files:**
- Modify: `firmware/src/core/hub_proto.cpp`, `firmware/src/core/hub_proto.h`
- Test: `firmware/test/test_hub_proto/test_main.cpp` (extend)

**Interfaces:**
- Produces: `bool hub_parse_sessions(const char* json, size_t len, buddy_rec_t* buddy, bool* had_sessions)` — fills `buddy->sessions[]` (≤ `BUDDY_SESSIONS_MAX`, label truncated, state mapped), sets `session_count`; preserves hub order (already sorted); returns false on invalid JSON / wrong version.

- [ ] **Step 1: Write the failing tests**

```cpp
void test_parse_sessions_basic(void) {
  const char* j = "{\"v\":1,\"sessions\":[" 
    "{\"id\":\"s3\",\"label\":\"beacon \xC2\xB7 fix/109\",\"state\":\"attention\",\"ts\":1719400000},"
    "{\"id\":\"s1\",\"label\":\"api \xC2\xB7 main\",\"state\":\"working\",\"ts\":1719399860}]}";
  buddy_rec_t b; memset(&b, 0, sizeof(b));
  bool had = false;
  TEST_ASSERT_TRUE(hub_parse_sessions(j, strlen(j), &b, &had));
  TEST_ASSERT_TRUE(had);
  TEST_ASSERT_EQUAL_UINT8(2, b.session_count);
  TEST_ASSERT_EQUAL_STRING("s3", b.sessions[0].id);
  TEST_ASSERT_EQUAL_UINT8(BST_ATTENTION, b.sessions[0].state);
  TEST_ASSERT_EQUAL_UINT8(BST_WORKING, b.sessions[1].state);
  TEST_ASSERT_EQUAL_UINT32(1719400000u, b.sessions[0].ts);
}

void test_parse_sessions_caps_and_truncates(void) {
  // 7 sessions, an over-length label, an unknown state.
  const char* j = "{\"v\":1,\"sessions\":["
    "{\"id\":\"s1\",\"label\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\"state\":\"glorp\",\"ts\":7},"
    "{\"id\":\"s2\",\"label\":\"x\",\"state\":\"idle\",\"ts\":6},"
    "{\"id\":\"s3\",\"label\":\"x\",\"state\":\"idle\",\"ts\":5},"
    "{\"id\":\"s4\",\"label\":\"x\",\"state\":\"idle\",\"ts\":4},"
    "{\"id\":\"s5\",\"label\":\"x\",\"state\":\"idle\",\"ts\":3},"
    "{\"id\":\"s6\",\"label\":\"x\",\"state\":\"idle\",\"ts\":2},"
    "{\"id\":\"s7\",\"label\":\"x\",\"state\":\"idle\",\"ts\":1}]}";
  buddy_rec_t b; memset(&b, 0, sizeof(b));
  bool had = false;
  TEST_ASSERT_TRUE(hub_parse_sessions(j, strlen(j), &b, &had));
  TEST_ASSERT_EQUAL_UINT8(BUDDY_SESSIONS_MAX, b.session_count);          // capped at 5
  TEST_ASSERT_TRUE(strlen(b.sessions[0].label) <= BUDDY_LABEL_LEN - 1);  // truncated
  TEST_ASSERT_EQUAL_UINT8(BST_WORKING, b.sessions[0].state);             // unknown => working
}

void test_parse_sessions_rejects_bad_version(void) {
  const char* j = "{\"v\":2,\"sessions\":[]}";
  buddy_rec_t b; memset(&b, 0, sizeof(b));
  bool had = false;
  TEST_ASSERT_FALSE(hub_parse_sessions(j, strlen(j), &b, &had));
}
```
Register them in the suite's `RUN_TEST` block.

- [ ] **Step 2: Run to verify failure**

Run: `cd firmware && ~/.beacon-pio/bin/pio test -e native -f "*test_hub_proto*"`
Expected: FAIL — `hub_parse_sessions` undefined.

- [ ] **Step 3: Implement the parser**

In `hub_proto.h`, declare:
```c
bool hub_parse_sessions(const char* json, size_t len, buddy_rec_t* buddy, bool* had_sessions);
```
In `hub_proto.cpp`, add (near `hub_parse_status`):
```cpp
static uint8_t map_session_state(const char* s) {
  if (!s) return BST_WORKING;
  if (!strcmp(s, "waiting"))        return BST_WAITING;
  if (!strcmp(s, "waiting_queued")) return BST_WAITING_QUEUED;
  if (!strcmp(s, "attention"))      return BST_ATTENTION;
  if (!strcmp(s, "idle"))           return BST_IDLE;
  return BST_WORKING;                                  // unknown => safest non-alerting state
}

bool hub_parse_sessions(const char* json, size_t len, buddy_rec_t* buddy, bool* had_sessions) {
  *had_sessions = false;
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return false;
  if ((doc["v"] | 0) != 1) return false;
  JsonVariantConst arr = doc["sessions"];
  if (!arr.is<JsonArrayConst>()) return false;
  *had_sessions = true;
  buddy->session_count = 0;
  for (JsonVariantConst s : arr.as<JsonArrayConst>()) {
    if (buddy->session_count >= BUDDY_SESSIONS_MAX) break;   // hub caps at 5; defend anyway
    buddy_session_t* d = &buddy->sessions[buddy->session_count];
    copy_trunc(d->id,    BUDDY_SID_LEN,   s["id"].as<const char*>());
    copy_trunc(d->label, BUDDY_LABEL_LEN, s["label"].as<const char*>());
    d->state = map_session_state(s["state"].as<const char*>());
    d->ts    = s["ts"] | (uint32_t)0;
    buddy->session_count++;
  }
  return true;
}
```

- [ ] **Step 4: Run to verify pass**

Run: `cd firmware && ~/.beacon-pio/bin/pio test -e native -f "*test_hub_proto*"`
Expected: PASS (all new + existing).

- [ ] **Step 5: Commit**

```bash
git add firmware/src/core/hub_proto.h firmware/src/core/hub_proto.cpp firmware/test/test_hub_proto/test_main.cpp
git commit -m "feat(firmware): parse standalone sessions frame"
```

---

### Task 9: DataStore merge (`ds_apply_sessions`) + frame dispatch

**Files:**
- Modify: `firmware/src/core/datastore.h`, `firmware/src/core/datastore.cpp` (native-tested), `firmware/src/core/hub_task.cpp` (compile-only — excluded from the native build)
- Test: `firmware/test/test_datastore/test_main.cpp`

**Note on test boundary:** `datastore.cpp` IS in the native `build_src_filter`; `hub_task.cpp` is NOT (it pulls Arduino/BLE). So `ds_apply_sessions` is unit-tested directly via the datastore setters/getters (no `ds_ingest_line` seam exists); the `on_frame` dispatch wiring in `hub_task.cpp` is verified by device build only.

**Interfaces:**
- Produces: `void ds_apply_sessions(const buddy_session_t* s, uint8_t count, uint32_t now)` — takes the datastore lock, copies ONLY `sessions[]`/`session_count` into the stored buddy record, stamps `hdr.last_updated = now`, sets `ST_LIVE`; leaves scalars/prompt intact (the sessions frame and buddy frame are independent). Mirrors `ds_set_buddy`'s lock/stamp pattern (`datastore.cpp:71`), but merges instead of overwriting.

- [ ] **Step 1: Write the failing test (merge does not clobber prompt) — drive the datastore directly**

In `firmware/test/test_datastore/test_main.cpp`:
```cpp
void test_apply_sessions_preserves_prompt(void) {
  ds_init();
  buddy_rec_t seed; memset(&seed, 0, sizeof(seed));
  seed.running = 1; seed.prompt.present = true;
  strcpy(seed.prompt.id, "p1"); strcpy(seed.prompt.tool, "Bash"); strcpy(seed.prompt.hint, "x");
  ds_set_buddy(&seed);                                   // buddy frame established the prompt

  buddy_session_t s[1]; memset(s, 0, sizeof(s));
  strcpy(s[0].id, "s1"); strcpy(s[0].label, "api"); s[0].state = BST_WORKING; s[0].ts = 9;
  ds_apply_sessions(s, 1, /*now=*/1000);                 // sessions frame merges in

  buddy_rec_t b = ds_get_buddy();
  TEST_ASSERT_TRUE(b.prompt.present);                    // prompt survived the merge
  TEST_ASSERT_EQUAL_UINT8(1, b.running);                 // scalars survived
  TEST_ASSERT_EQUAL_UINT8(1, b.session_count);
  TEST_ASSERT_EQUAL_STRING("s1", b.sessions[0].id);
}
```
Register it in this suite's `RUN_TEST` block.

- [ ] **Step 2: Run to verify failure**

Run: `cd firmware && ~/.beacon-pio/bin/pio test -e native -f "*test_datastore*"`
Expected: FAIL — `ds_apply_sessions` undefined.

- [ ] **Step 3a: Implement the merge in `datastore.cpp` (declare in `datastore.h`)**

```c
// In datastore.h:
void ds_apply_sessions(const buddy_session_t* s, uint8_t count, uint32_t now);
```
```cpp
// In datastore.cpp (mirror ds_set_buddy's lock + stamp; MERGE, do not overwrite):
void ds_apply_sessions(const buddy_session_t* s, uint8_t count, uint32_t now) {
  if (count > BUDDY_SESSIONS_MAX) count = BUDDY_SESSIONS_MAX;
  ds_lock_take(s_lock);
  s_buddy.session_count = count;
  for (uint8_t i = 0; i < count; i++) s_buddy.sessions[i] = s[i];
  s_buddy.hdr.last_updated = now;
  s_buddy.hdr.state = ST_LIVE;
  s_buddy.hdr.err = ERR_NONE;
  ds_lock_give(s_lock);
}
```

- [ ] **Step 3b: Wire the dispatch in `hub_task.cpp`'s `on_frame(const char* json, size_t len)`**

Before the `hub_parse_status` fall-through (after the `hub_parse_loc` branch, ~line 137), add — note the real callback params are `json`/`len`, and `now` is already computed below as `timekeep_now()`; compute it up front for this branch:
```cpp
  { buddy_rec_t tmp; bool had = false;
    if (hub_parse_sessions(json, len, &tmp, &had) && had) {
      ds_apply_sessions(tmp.sessions, tmp.session_count, (uint32_t)timekeep_now());
      return;
    } }
```

- [ ] **Step 4: Run to verify pass + device build**

Run: `cd firmware && ~/.beacon-pio/bin/pio test -e native -f "*test_datastore*" && ~/.beacon-pio/bin/pio test -e native && ~/.beacon-pio/bin/pio run`
Expected: native suite green; device build compiles (proves the `hub_task.cpp` dispatch wiring).

- [ ] **Step 5: Commit**

```bash
git add firmware/src/core/datastore.h firmware/src/core/datastore.cpp firmware/src/core/hub_task.cpp firmware/test/test_datastore/test_main.cpp
git commit -m "feat(firmware): merge sessions frame into buddy record"
```

---

### Task 10: Session-list UI — shared helpers + reference theme (calm)

**Files:**
- Modify: `firmware/src/ui/screens/views/view_common.h`, `firmware/src/ui/screens/views/buddy_calm.cpp`

**Interfaces:**
- Produces: `buddy_session_state_color(const beacon_theme_t*, uint8_t state) -> lv_color_t`; `buddy_session_glyph(uint8_t state) -> const char*`; `buddy_session_age(uint32_t ts, char* out, size_t n)` (writes `"2m"`/`"now"`/`""` when no clock); `buddy_session_split_label(const char* label, char* folder, size_t fn, char* branch, size_t bn)` (splits `"folder · branch"` on the `·` separator for the two-line row; `branch` empty when none).

- [ ] **Step 1: Add shared helpers to `view_common.h`**

```c
#include "ui/theme.h"
// State cue: accent reserved for the session that needs you (attention); waiting amber via theme down;
// working ink; queued/idle dim. (Editorial "one accent" honored — see DESIGN.md.)
static inline lv_color_t buddy_session_state_color(const beacon_theme_t* t, uint8_t st) {
  switch (st) {
    case BST_ATTENTION:      return t->accent;
    case BST_WAITING:        return t->down;       // amber/alert
    case BST_WORKING:        return t->ink;
    default:                 return t->ink_dim;    // queued / idle
  }
}
static inline const char* buddy_session_glyph(uint8_t st) {
  switch (st) { case BST_ATTENTION: return "*"; case BST_WAITING: return "!";
                case BST_WAITING_QUEUED: return "."; case BST_WORKING: return ">"; default: return "-"; }
}
// Relative age from a hub epoch ts using the device's synced wall clock. Empty when clock unsynced
// (design §5: never render a garbage delta) or ts==0.
static inline void buddy_session_age(uint32_t ts, char* out, size_t n) {
  if (!timekeep_has_time() || ts == 0) { out[0] = '\0'; return; }
  time_t now = time(NULL);
  long d = (long)now - (long)ts;
  if (d < 5)        snprintf(out, n, "now");
  else if (d < 60)  snprintf(out, n, "%lds", d);
  else if (d < 3600) snprintf(out, n, "%ldm", d / 60);
  else              snprintf(out, n, "%ldh", d / 3600);
}
// Split the hub label "folder · branch" into its two parts for the row's two lines. The separator is
// the UTF-8 middle dot "\xC2\xB7"; absent => the whole label is the folder and branch is empty.
static inline void buddy_session_split_label(const char* label, char* folder, size_t fn,
                                             char* branch, size_t bn) {
  const char* sep = strstr(label ? label : "", "\xC2\xB7");
  if (!sep) { snprintf(folder, fn, "%s", label ? label : ""); branch[0] = '\0'; return; }
  size_t flen = (size_t)(sep - label);
  while (flen > 0 && label[flen - 1] == ' ') flen--;            // trim trailing space before the dot
  snprintf(folder, fn, "%.*s", (int)flen, label);
  const char* b = sep + 2;                                       // skip the 2-byte separator
  while (*b == ' ') b++;                                         // trim leading space after the dot
  snprintf(branch, bn, "%s", b);
}
```

- [ ] **Step 2: Replace the idle-entries render in `buddy_calm.cpp`**

Replace the `s_idle[BUDDY_ENTRIES]` stack with a session-row stack (max 4 visible). In `build()`, create 4 row groups (folder label + branch label + state word + age), aligned within the safe area, 64 px row pitch. In `update()`, when `!b.prompt.present`, render the first `min(b.session_count, 4)` sessions: per row call `buddy_session_split_label(b.sessions[i].label, …)` for the folder/branch lines, `buddy_session_age(b.sessions[i].ts, …)` for the age, and `buddy_session_state_color(t, b.sessions[i].state)` for the cue; show an "idle"/"no sessions" empty state when `session_count == 0`; keep the prompt path untouched. (Row layout follows the existing `buddy_calm` style — `t->f_body` for folder, `t->f_mono` dim for branch/age.)

- [ ] **Step 3: Build for device + flash + visual check**

Run: `cd firmware && ~/.beacon-pio/bin/pio run`
Expected: compiles. Flash (`-t upload`) and confirm against `docs/design/mockups/claude-sessions.html`: 0/1/many sessions, attention accent, age suppressed before NTP sync.

- [ ] **Step 4: Commit**

```bash
git add firmware/src/ui/screens/views/view_common.h firmware/src/ui/screens/views/buddy_calm.cpp
git commit -m "feat(firmware): session-list render on claude screen (calm theme)"
```

---

### Task 11: Apply the session list to the remaining 6 themes

**Files:**
- Modify: `firmware/src/ui/screens/views/buddy_{editorial,hud,led,analog,blueprint,oscilloscope}.cpp`

**Interfaces:**
- Consumes: the `view_common.h` helpers (Task 10).

- [ ] **Step 1: Port each theme (one commit per theme to keep reviews atomic)**

For each of the 6 views, apply the same transform as Task 10 within that theme's existing tokens/layout: replace the idle-entries stack with up to 4 session rows; use `buddy_session_state_color`/`buddy_session_glyph`/`buddy_session_age`; keep each theme's typographic treatment (e.g. `hud` boxed rows, `editorial` hairline rules, `oscilloscope` trace styling). Render the empty/single-session states per theme. Do not touch the prompt path.

- [ ] **Step 2: Build + per-theme visual check**

Run: `cd firmware && ~/.beacon-pio/bin/pio run`
Expected: compiles. Flash and cycle the theme setting; verify each theme's `claude` screen shows the 4-row list inside the safe area with correct state cues.

- [ ] **Step 3: Commit (per theme)**

```bash
git add firmware/src/ui/screens/views/buddy_editorial.cpp
git commit -m "feat(firmware): session list — editorial theme"
# repeat for hud, led, analog, blueprint, oscilloscope
```

---

### Task 12: Contract + DESIGN docs

**Files:**
- Modify: `hub/CONTRACT.md`, `DESIGN.md`

- [ ] **Step 1: Document the `sessions` frame + `open` command in CONTRACT.md**

In §A add the standalone `sessions` frame shape + caps (≤5, label ≤28, id ≤6, state enum) and the note that `buddy`/`entries` is unchanged for back-compat. In §B add the `open` command + ack/err (Phase 2; mark as such). State the worst-case `< 1024 B` guarantee.

- [ ] **Step 2: Add the session-row height note to DESIGN.md**

In the safe-area/list-row section, note: the `claude` session list shows **up to 4 rows** at ~64 px pitch within the 386 px safe content height; rows follow the list-row pattern; the single accent marks `attention`.

- [ ] **Step 3: Commit**

```bash
git add hub/CONTRACT.md DESIGN.md
git commit -m "docs: spec sessions frame + session-row layout"
```

---

### Task 13: Reconcile the permission fail-closed timeout drift (pre-existing cleanup)

**Files:**
- Modify: `hub/CONTRACT.md`, `docs/tech.md` (and a code comment in `firmware/src/core/records.h` / `ClaudeCodeBridge.swift` if the chosen number differs)

**Interfaces:** none (doc/constant alignment).

- [ ] **Step 1: Pick the authoritative number and align**

The code uses **590 s** (`records.h:69` `BUDDY_PROMPT_EXPIRY_S`, `ClaudeCodeBridge.swift` cap). CONTRACT.md §B says 25 s; tech.md §8 says ~30 s. Update CONTRACT.md and tech.md to state the real 590 s fail-closed cap (aligned to CC's ~600 s hook hold), or, if a shorter device-facing countdown is intended, reconcile both docs + the constant to one value. Keep `SESSION_IDLE_TTL` (300 s) documented as a separate concept.

- [ ] **Step 2: Commit**

```bash
git add hub/CONTRACT.md docs/tech.md
git commit -m "docs: reconcile permission fail-closed timeout (25/30 -> 590s)"
```

---

# PHASE 2 — Tap-to-open (spike-gated)

### Task 14: SPIKE — host-context capture + per-app focus

**Files:**
- Create: `docs/spikes/2026-XX-XX-session-focus-spike.md`

- [ ] **Step 1: Capture what a `SessionStart` hook subprocess can actually see**

Write a throwaway hook that dumps `$TERM_PROGRAM`, `$TERM_SESSION_ID`, `$TTY`/`tty`, `$PPID`, `$WINDOWID`, and the CC payload, run it from **each** host app: iTerm2, Apple Terminal, VS Code, Cursor, Ghostty, Warp. Record per-app which identifiers are present and stable.

- [ ] **Step 2: Prove a focus path per app**

For each app, validate a focus command: iTerm2 AppleScript select-by-`TERM_SESSION_ID`; Apple Terminal AppleScript select-by-`tty`; VS Code/Cursor `code -r <folder>` / `cursor -r <folder>`; Ghostty/Warp `open -a`. Record whether macOS Automation/Accessibility permission is required and that a denied/missing permission **fails fast** (no hang).

- [ ] **Step 3: Write findings + the resulting focus-tier table**

Document the reachable identifiers and the chosen focus command per app. This output parameterizes Tasks 15-18. Commit the spike doc.

```bash
git add docs/spikes/2026-XX-XX-session-focus-spike.md
git commit -m "docs(spike): session host-context capture + focus feasibility"
```

---

### Task 15: SessionStart host-context POST + installer matchers + registry fields

**Files:**
- Modify: hooks installer (`hub/build-app.sh` install-hooks / `hub/claude-code-settings.snippet.json`), `hub/Sources/beacon-hub/ClaudeCodeBridge.swift`, `hub/Sources/BeaconHubKit/SessionRegistry.swift`

- [ ] **Step 1: Extend the installer**

Add `clear|compact` to the `SessionStart` matcher (today `startup|resume`); add a host-context capture to the SessionStart hook command that POSTs the spike-confirmed env vars to `/hook` (or a new `/session` route).

- [ ] **Step 2: Store host context on the registry**

Add `host_app`, `tab` (the spike's stable identifier), `pid` to `SessionRegistry.Entry` via `setHost(sessionId:host:tab:pid:)`. Table-test that `setHost` + `touchActivity` keep the entry consistent across `clear`/`compact` (same `session_id` keeps its `s<n>`).

- [ ] **Step 3: Commit**

```bash
git add hub/build-app.sh hub/claude-code-settings.snippet.json hub/Sources/beacon-hub/ClaudeCodeBridge.swift hub/Sources/BeaconHubKit/SessionRegistry.swift hub/Tests/BeaconHubKitTests/SessionRegistryTests.swift
git commit -m "feat(hub): capture session host context at SessionStart"
```

---

### Task 16: `open` command — wire both sides

**Files:**
- Modify: `hub/Sources/BeaconHubKit/Protocol.swift` (parse), `hub/Sources/beacon-hub/AppDelegate.swift` (handle), `firmware/src/core/hub_proto.cpp/.h` (`hub_build_open`), `firmware/src/core/hub_task.cpp` (`buddy_open`/`hub_send_open`)
- Test: `hub/Tests/BeaconHubKitTests/...` + `firmware/test/test_hub_proto/`

- [ ] **Step 1 (hub, failing test): parse `open`**

```swift
func testParsesOpenCommand() {
    let d = Data("{\"v\":1,\"cmd\":\"open\",\"id\":\"s3\"}".utf8)
    XCTAssertEqual(DeviceCommand.parse(d), .open(id: "s3"))
}
```
Add to `DeviceCommand`: `case open(id: String)` and in `parse`:
```swift
        case "open":
            guard let id = obj["id"] as? String, !id.isEmpty else { return nil }
            return .open(id: id)
```

- [ ] **Step 2 (firmware, failing test): build `open` frame**

```cpp
void test_build_open(void) {
  char buf[64];
  size_t n = hub_build_open(buf, sizeof(buf), "s3");
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"cmd\":\"open\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"id\":\"s3\""));
  TEST_ASSERT_EQUAL_CHAR('\n', buf[n-1]);
}
```
Implement `hub_build_open` mirroring `hub_build_permission` (cmd `open`, `id` only).

- [ ] **Step 3: Handle on the hub (AppDelegate) + send on the device (hub_task)**

In the central's command callback (`handle(cmd:)`, AppDelegate line 208, where `.permission` is handled), add `.open(id:)`: look the `s<id>` up in the registry's host context (Task 15). Unknown id → `central.send(HubAck.err(id: id, reason: "unknown_session"))`. Known id → `central.send(HubAck.ack(id: id, ok: true))` as a **placeholder ack**; Task 17 replaces this with the real focus result. (This keeps Task 16 independently testable before the resolver exists — Codex B11.)

On the device, add `bool buddy_open(const char* id)` → `hub_send_open(id)` enqueuing the frame (mirror `buddy_decide`/`hub_send_permission`).

- [ ] **Step 4: Run tests + build**

Run: `cd hub && swift test` and `cd firmware && ~/.beacon-pio/bin/pio test -e native -f "*test_hub_proto*"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add hub/Sources/BeaconHubKit/Protocol.swift firmware/src/core/hub_proto.cpp firmware/src/core/hub_proto.h firmware/src/core/hub_task.cpp firmware/test/test_hub_proto/test_main.cpp
git commit -m "feat: add open device command (wire both sides)"
```

---

### Task 17: Focus resolver (hub) — tiered best-effort

**Files:**
- Create: `hub/Sources/beacon-hub/SessionFocus.swift`
- Modify: `hub/Sources/beacon-hub/AppDelegate.swift`

- [ ] **Step 1: Implement the tiers from the spike findings**

`func focus(_ host: SessionHostContext) -> Bool` dispatches by `host_app` to the spike-validated command (iTerm2/Terminal AppleScript via `NSAppleScript`/`osascript`; VS Code/Cursor `code -r`/`cursor -r`; Ghostty/Warp `open -a`). Run off the main thread; **time-box** each attempt; return success/failure for the ack. Any Automation/Accessibility denial returns `false` fast (never blocks). Then **replace the Task 16 placeholder ack** in `handle(cmd:)`: for a known id, call `focus(host)` and send `HubAck.ack(id: id, ok: focus(host))` (compute once), so the device sees a truthful ok/err.

- [ ] **Step 2: Manual verification per tier**

Tap each app's session on the device; confirm: iTerm2/Terminal focus the exact tab, VS Code/Cursor focus the repo window, Ghostty/Warp bring the app forward; a revoked Automation permission yields a device "couldn't open" (ack ok:false), not a hang.

- [ ] **Step 3: Commit**

```bash
git add hub/Sources/beacon-hub/SessionFocus.swift hub/Sources/beacon-hub/AppDelegate.swift
git commit -m "feat(hub): tiered best-effort session focus resolver"
```

---

### Task 18: Device tap + pinned feedback (all 7 themes)

**Files:**
- Modify: `firmware/src/ui/screens/views/view_common.h`, all 7 `buddy_*.cpp`

- [ ] **Step 1: Make rows tappable + pin feedback by id**

Add a tap handler per session row that calls `buddy_open(b.sessions[i].id)` and sets a device-local feedback state keyed by **session id** (`opening` → `ok`/`err`), pinned until ack/err/timeout and **immune to frame re-sort** (look up the pinned id in the next frame, not the row index). Mirror the prompt `PROMPT_SENT_OK`/`PROMPT_TOO_LATE` confirm-hold pattern. Add the ack/err routing for `s<n>` ids in `hub_task` (extend the existing `hub_apply_ack` path or add a sibling for session acks).

- [ ] **Step 2: Build + manual check**

Run: `cd firmware && ~/.beacon-pio/bin/pio run`
Expected: compiles. Tap a row → "opening…" pins through a re-sort → resolves ok/err.

- [ ] **Step 3: Commit**

```bash
git add firmware/src/ui/screens/views/ firmware/src/core/hub_task.cpp
git commit -m "feat(firmware): tap-to-open session rows with pinned feedback"
```

---

# PHASE 3 — Device audio (spike-gated)

### Task 19: SPIKE — ES8311 + I2S chime under load

**Files:**
- Create: `docs/spikes/2026-XX-XX-device-audio-heap-spike.md`

- [ ] **Step 1: Prove the heap floor holds**

Init ES8311 (I2C) + I2S, play one short chime, under the worst case (active bonded BLE + cert TLS fetch + full LVGL on the heaviest theme). Log `esp_get_free_internal_heap_size()` min-free every second for several minutes across repeated chimes + TLS fetches.

- [ ] **Step 2: Decide**

If min-free stays **≥ 60 KB** with margin and TLS fetches keep succeeding → device audio is viable (proceed to Task 20). If it dips toward ~53 KB or TLS fails → **stop**; keep audio hub-only and record the result. Commit the spike doc either way.

```bash
git add docs/spikes/2026-XX-XX-device-audio-heap-spike.md
git commit -m "docs(spike): device audio heap feasibility under BLE+TLS+LVGL"
```

---

### Task 20: Device chimes (only if Task 19 passed)

**Files:**
- Create: `firmware/src/hal/audio.h` / `audio.cpp`
- Modify: `firmware/src/config/pins.h`, the buddy state-transition path

- [ ] **Step 1: Add the audio HAL**

Define the I2S pins (from the board's routed ES8311 lines), init ES8311 + I2S in the boot sequence (after the existing rails, gated so a missing codec degrades silently), expose `audio_play(sound_id)` reading short PCM clips from SPIFFS.

- [ ] **Step 2: Trigger on transitions**

Play a distinct device chime on the same `waiting`/`attention` transitions the hub uses (mirror the hub debounce so a device chime doesn't double with the Mac when both are enabled; gate behind a setting).

- [ ] **Step 3: Build + on-device verify + heap watch**

Run: `cd firmware && ~/.beacon-pio/bin/pio run`
Expected: compiles. Flash; confirm chimes play and the logged min-free internal heap stays ≥ 60 KB under load.

- [ ] **Step 4: Commit**

```bash
git add firmware/src/hal/audio.h firmware/src/hal/audio.cpp firmware/src/config/pins.h firmware/src/core/
git commit -m "feat(firmware): device-side session chimes via ES8311"
```

---

## Self-Review (author checklist — done)

- **Spec coverage:** state model → T2/T3/T6; standalone sessions frame + caps → T1/T8/T12; registry + `s<n>` + idle TTL → T2; branch async → T4; sound + debounce → T6; device 7-theme list (4 rows, age skew, 0/1-session) → T7-T11; tap-to-open spike+build → T14-T18; device-audio spike+build → T19-T20; timeout-drift cleanup → T13; migration (separate frame) → T1/T9/T12. No uncovered spec section.
- **Placeholders:** none — Phase 1 carries real code; Phase 2/3 spike-dependent specifics (AppleScript per app, I2S register init) are explicitly produced by Tasks 14/19 before their build tasks, which is correct sequencing, not a placeholder.
- **Type consistency:** `Session`/`SessionState`/`SessionLimits`/`SessionsFrame` (T1) reused verbatim in T2/T3/T5/T6; `buddy_session_t`/`BST_*`/`BUDDY_SESSIONS_MAX` (T7) reused in T8/T9/T10; `onSessionsUpdate`/`onAttention` (T3/T6) consumed in T5/T6; `hub_parse_sessions` (T8) consumed in T9; `buddy_open`/`hub_build_open` (T16) consumed in T18.
