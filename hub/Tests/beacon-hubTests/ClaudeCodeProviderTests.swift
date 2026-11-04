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

    // #155: rate_limits without a 5h/7d window (e.g. gateway spend_limit only) is not a Claude usage
    // source, so it must neither emit a value nor refresh the liveness that suppresses the oauth poll.
    func testStatuslineWithoutSubscriberWindowsIsIgnored() {
        let (p, _) = makeProvider()
        var activityCount = 0, usageCount = 0
        p.onStatuslineActivity = { activityCount += 1 }
        p.onClaudeUsage = { _ in usageCount += 1 }
        p.handleStatusline(["session_id": "s1",
                            "rate_limits": ["spend_limit": ["used_percentage": 63, "resets_at": 1_740_787_200]]])
        drainMain()
        XCTAssertEqual(activityCount, 0)
        XCTAssertEqual(usageCount, 0)
    }

    // Device offline => pass-through (no verdict, "{}"), never a deny: the user never saw the prompt,
    // so the harness must ask on the Mac instead of failing the tool call. No prompt raised to the mux.
    func testOfflinePassesThroughWithoutRaising() {
        let (p, sink) = makeProvider(deviceConnected: false)
        var responded: Data?
        var undeliverable: String?
        p.onPromptUndeliverable = { undeliverable = $0 }
        p.handlePermissionForTest(body: ["hook_event_name": "PermissionRequest", "session_id": "z",
                                         "tool_name": "Bash", "tool_input": ["command": "rm -rf /"]],
                                  respond: { data, _ in responded = data })
        drainMain()
        XCTAssertEqual(responded, HookResponse.permissionAsk(event: "PermissionRequest"))
        XCTAssertEqual(String(decoding: responded ?? Data(), as: UTF8.self), "{}")
        XCTAssertEqual(undeliverable, "Beacon offline - CLAUDE not gated", "the Mac still gets a menubar alert")
        XCTAssertTrue(sink.raises.isEmpty, "offline prompt must not be raised")
        XCTAssertEqual(p.heldCountForTest(), 0)
    }

    // Link drops while a prompt is held: it can never be answered on the device, so release it
    // pass-through immediately instead of letting the 590 s cap deny it.
    func testDisconnectReleasesHeldPromptsPassthrough() {
        let (p, sink) = makeProvider()
        var responded: Data?
        p.handlePermissionForTest(body: ["hook_event_name": "PermissionRequest", "session_id": "z",
                                         "tool_name": "Bash"],
                                  respond: { data, _ in responded = data })
        drainMain()
        XCTAssertNil(responded, "held while connected")
        let nativeID = p.lastNativeIdForTest()!

        p.setDeviceConnected(false)
        XCTAssertEqual(p.heldCountForTest(), 0)   // queue.sync: flushes the disconnect hop
        drainMain()
        XCTAssertEqual(String(decoding: responded ?? Data(), as: UTF8.self), "{}", "no verdict => passthrough")
        XCTAssertEqual(sink.ends, [nativeID])
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

    // CC 2.1.318 sends notification_type on every Notification; only the ones that wait on the user may
    // mark the session as asking a question. Absent (older CC) and unknown types keep needs-input.
    func testNotificationTypeDecidesNeedsInput() {
        let cases: [(type: String?, needsInput: Bool)] = [
            (nil, true),
            ("permission_prompt", true),
            ("idle_prompt", true),
            ("elicitation_dialog", true),
            ("agent_needs_input", true),
            ("some_future_type", true),
            ("auth_success", false),
            ("elicitation_complete", false),
            ("elicitation_response", false),
            ("agent_completed", false),
            ("quota_auto_resume_fired", false),
            ("plugin_notification", false),
        ]
        for c in cases {
            let (p, sink) = makeProvider()
            var extra: [String: Any] = ["message": "Claude needs your permission"]
            if let type = c.type { extra["notification_type"] = type }
            p.applySessionHookForTest(event: "Notification", sessionId: "n1", cwd: "/x/api", extra: extra)
            drainMain()
            let expected: [ProviderSessionEvent] = c.needsInput ? [.needsInput(nativeKey: "n1", cwd: "/x/api")] : []
            XCTAssertEqual(sink.sessions, expected, "notification_type=\(c.type ?? "nil")")
            XCTAssertEqual(sink.entries.count, 1, "every Notification still logs an activity entry")
        }
    }

    // CC reports a forked session (`--fork-session`, `/branch`) as SessionStart source "fork" (since CC
    // 2.1.214; it was "resume" before), so the installed matchers must name it or a fork never reaches
    // the hub's SessionStart handlers (host context for tap-to-focus included).
    func testSnippetSessionStartMatchersCoverForkedSessions() throws {
        let url = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("claude-code-settings.snippet.json")
        let snippet = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf: url)) as? [String: Any])
        let wrappers = try XCTUnwrap((snippet["hooks"] as? [String: Any])?["SessionStart"] as? [[String: Any]])
        XCTAssertFalse(wrappers.isEmpty)
        for w in wrappers {
            let sources = Set(((w["matcher"] as? String) ?? "").split(separator: "|").map(String.init))
            XCTAssertTrue(sources.isSuperset(of: ["startup", "resume", "fork"]), "matcher \(sources)")
        }
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
