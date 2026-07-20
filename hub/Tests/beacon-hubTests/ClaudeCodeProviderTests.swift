import XCTest
import BeaconHubKit
@testable import beacon_hub

// Provider-level behavior that stays in ClaudeCodeProvider after the mux refactor: statusline usage
// dedup/liveness, prompt holding + the fail-closed decision paths, drain, and toggle pass-through. The
// FIFO/qlen/session-state behaviors moved to ProviderMux (ProviderMuxTests covers them).
final class ClaudeCodeProviderTests: XCTestCase {

    // Captures the mux-facing events; the provider hops sink calls to the main queue.
    private final class MockSink: ProviderSink {
        var raises: [(id: String, nativeID: String, tool: String, hint: String, sessionKey: String?)] = []
        var ends: [String] = []
        var sessions: [ProviderSessionEvent] = []
        var entries: [String] = []
        func provider(_ id: String, didUpdateUsage usage: ProviderUsage) {}
        func provider(_ id: String, didUpdateMetrics tokens: Int, contextPct: Int) {}
        func provider(_ id: String, didUpdateSession event: ProviderSessionEvent) { sessions.append(event) }
        func provider(_ id: String, didRaisePrompt nativeID: String, tool: String, hint: String, sessionNativeKey: String?) {
            raises.append((id, nativeID, tool, hint, sessionNativeKey))
        }
        func provider(_ id: String, didEndPrompt nativeID: String) { ends.append(nativeID) }
        func provider(_ id: String, didAppendEntry line: String) { entries.append(line) }
    }

    private func makeProvider(deviceConnected: Bool = true) -> (ClaudeCodeProvider, MockSink) {
        let p = ClaudeCodeProvider(server: LocalIngestServer())
        let sink = MockSink()
        p.start(sink: sink)
        p.setDeviceConnected(deviceConnected)
        return (p, sink)
    }

    private func drainMain() {
        let e = expectation(description: "main"); DispatchQueue.main.async { e.fulfill() }
        wait(for: [e], timeout: 1)
    }

    // #93/#59: a rate_limits-carrying POST proves Claude Code is alive regardless of whether the parsed
    // value changed, so liveness fires on EVERY such POST while onClaudeUsage stays deduped.
    func testStatuslineActivityFiresEveryPostWhileUsageDedups() {
        let (p, _) = makeProvider()
        var activityCount = 0, usageCount = 0
        p.onStatuslineActivity = { activityCount += 1 }
        p.onClaudeUsage = { _ in usageCount += 1 }
        let body: [String: Any] = [
            "session_id": "s1",
            "rate_limits": [
                "five_hour": ["used_percentage": 12, "resets_at": 1_700_000_000],
                "seven_day": ["used_percentage": 34, "resets_at": 1_700_500_000],
            ],
        ]
        p.handleStatusline(body)
        p.handleStatusline(body)
        drainMain()
        XCTAssertEqual(activityCount, 2, "liveness must fire on every rate_limits POST")
        XCTAssertEqual(usageCount, 1, "unchanged value must stay deduped (#59)")
    }

    // Device offline => deny immediately (named), never hold; no prompt raised to the mux.
    func testOfflineDeniesImmediatelyWithoutRaising() {
        let (p, sink) = makeProvider(deviceConnected: false)
        var responded: Data?
        p.handlePermissionForTest(body: ["hook_event_name": "PermissionRequest", "session_id": "z",
                                         "tool_name": "Bash", "tool_input": ["command": "rm -rf /"]],
                                  respond: { data, _ in responded = data })
        drainMain()
        XCTAssertNotNil(responded)
        let obj = try! JSONSerialization.jsonObject(with: responded!) as! [String: Any]
        let decision = ((obj["hookSpecificOutput"] as! [String: Any])["decision"] as! [String: Any])
        XCTAssertEqual(decision["behavior"] as? String, "deny")
        XCTAssertTrue(sink.raises.isEmpty, "offline prompt must not be raised")
        XCTAssertEqual(p.heldCountForTest(), 0)
    }

    // Buddy toggle OFF => pass-through (no verdict, {} for PermissionRequest); never auto-deny, never hold.
    func testBuddyOffPassesThroughWithoutVerdict() {
        let (p, sink) = makeProvider()
        p.setEnabled(EnabledCapabilities(usage: true, buddy: false))
        var responded: Data?
        p.handlePermissionForTest(body: ["hook_event_name": "PermissionRequest", "session_id": "z",
                                         "tool_name": "Bash"],
                                  respond: { data, _ in responded = data })
        drainMain()
        XCTAssertEqual(String(decoding: responded ?? Data(), as: UTF8.self), "{}", "no verdict => passthrough")
        XCTAssertTrue(sink.raises.isEmpty)
        XCTAssertEqual(p.heldCountForTest(), 0)
    }

    // A deliverable prompt is HELD (no response) and raised to the mux; resolving it fulfills the
    // connection and ends the prompt.
    func testHeldPromptRaisesThenResolves() {
        let (p, sink) = makeProvider()
        var responded: Data?
        p.handlePermissionForTest(body: ["hook_event_name": "PermissionRequest", "session_id": "z",
                                         "tool_name": "Bash", "tool_input": ["command": "ls"]],
                                  respond: { data, _ in responded = data })
        drainMain()
        XCTAssertNil(responded, "a deliverable prompt is held, not answered yet")
        XCTAssertEqual(sink.raises.count, 1)
        XCTAssertEqual(sink.raises.first?.sessionKey, "z")
        let nativeID = p.lastNativeIdForTest()!

        XCTAssertEqual(p.resolvePrompt(nativeID: nativeID, approve: true), .applied)
        drainMain()
        XCTAssertNotNil(responded, "resolving fulfills the held connection")
        XCTAssertEqual(sink.ends, [nativeID])
        // A second decision for the same id is late (already resolved).
        XCTAssertEqual(p.resolvePrompt(nativeID: nativeID, approve: true), .unknown)
    }

    // AskUserQuestion is passed through (never held) and surfaces a passive activity entry.
    func testAskUserQuestionPassthrough() {
        let (p, sink) = makeProvider()
        var responded: Data?
        p.handlePermissionForTest(body: ["hook_event_name": "PermissionRequest", "session_id": "z",
                                         "tool_name": "AskUserQuestion", "cwd": "/x/api"],
                                  respond: { data, _ in responded = data })
        drainMain()
        XCTAssertEqual(String(decoding: responded ?? Data(), as: UTF8.self), "{}")
        XCTAssertTrue(sink.raises.isEmpty)
        XCTAssertFalse(sink.entries.isEmpty, "a passive 'asking a question' entry is surfaced")
    }

    // Quit drain denies every held prompt and clears the queue.
    func testDrainDeniesEveryHeldPrompt() {
        let (p, _) = makeProvider()
        p.injectPermissionForTest(sessionId: "t1", tool: "Bash", hint: "one")
        p.injectPermissionForTest(sessionId: "t2", tool: "Write", hint: "two")
        XCTAssertEqual(p.heldCountForTest(), 2)
        let drained = expectation(description: "drained")
        p.drainHeldPrompts(reason: "quitting") { drained.fulfill() }
        wait(for: [drained], timeout: 1)
        XCTAssertEqual(p.heldCountForTest(), 0, "drain clears the whole queue")
    }
}
