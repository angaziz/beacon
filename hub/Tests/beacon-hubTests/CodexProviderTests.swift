import XCTest
import BeaconHubKit
@testable import beacon_hub

// Codex buddy adapter: event -> session-state mapping and the PermissionRequest decision paths
// (buddy-off pass-through, offline deny, held resolve, quit drain). Mirrors ClaudeCodeProviderTests;
// the FIFO/qlen/session-state aggregation lives in ProviderMux (its own tests).
final class CodexProviderTests: XCTestCase {

    private final class MockSink: ProviderSink {
        var raises: [(nativeID: String, tool: String, hint: String, sessionKey: String?)] = []
        var ends: [String] = []
        var sessions: [ProviderSessionEvent] = []
        func provider(_ id: String, didUpdateUsage usage: ProviderUsage) {}
        func provider(_ id: String, didUpdateMetrics tokens: Int, contextPct: Int) {}
        func provider(_ id: String, didUpdateSession event: ProviderSessionEvent) { sessions.append(event) }
        func provider(_ id: String, didRaisePrompt nativeID: String, tool: String, hint: String, sessionNativeKey: String?) {
            raises.append((nativeID, tool, hint, sessionNativeKey))
        }
        func provider(_ id: String, didEndPrompt nativeID: String) { ends.append(nativeID) }
        func provider(_ id: String, didAppendEntry line: String) {}
    }

    private func makeProvider(deviceConnected: Bool = true) -> (CodexProvider, MockSink) {
        let p = CodexProvider(server: LocalIngestServer())
        let sink = MockSink()
        p.branchResolverForTest = { _ in nil }   // never shell git in tests
        p.start(sink: sink)
        p.setDeviceConnected(deviceConnected)
        return (p, sink)
    }

    private func drainMain() {
        let e = expectation(description: "main"); DispatchQueue.main.async { e.fulfill() }
        wait(for: [e], timeout: 1)
    }

    // hook_event_name -> ProviderSessionEvent: lifecycle events map to the shared registry transitions.
    func testEventToSessionMapping() {
        let cases: [(event: String, expect: (ProviderSessionEvent) -> Bool)] = [
            ("SessionStart",     { if case .activity(let k, _) = $0 { return k == "s1" } else { return false } }),
            ("UserPromptSubmit", { if case .activity(let k, _) = $0 { return k == "s1" } else { return false } }),
            ("Stop",             { if case .stop(let k, _) = $0 { return k == "s1" } else { return false } }),
            ("SessionEnd",       { if case .end(let k) = $0 { return k == "s1" } else { return false } }),
        ]
        for c in cases {
            let (p, sink) = makeProvider()
            p.applySessionHookForTest(event: c.event, sessionId: "s1", cwd: "/tmp/proj")
            drainMain()
            let first = sink.sessions.first
            XCTAssertNotNil(first, "\(c.event): no session event emitted")
            XCTAssertTrue(first.map(c.expect) ?? false, "\(c.event): wrong mapping (\(String(describing: first)))")
        }
    }

    // Buddy toggle OFF => pass-through (no verdict, "{}"); never auto-deny, never hold, never raise.
    func testBuddyOffPassesThroughWithoutVerdict() {
        let (p, sink) = makeProvider()
        p.setEnabled(EnabledCapabilities(usage: true, buddy: false))
        var body: Data?
        p.handlePermissionForTest(body: ["hook_event_name": "PermissionRequest", "tool_name": "Bash"]) { d, _ in body = d }
        drainMain()
        XCTAssertEqual(body, HookResponse.permissionAsk(event: "PermissionRequest"))
        XCTAssertEqual(body, Data("{}".utf8))
        XCTAssertEqual(p.heldCountForTest(), 0)
        XCTAssertTrue(sink.raises.isEmpty)
    }

    // Device offline => deny immediately (named), never hold; no prompt raised.
    func testOfflineDeniesImmediatelyWithoutRaising() {
        let (p, sink) = makeProvider(deviceConnected: false)
        var body: Data?
        p.handlePermissionForTest(body: ["hook_event_name": "PermissionRequest", "tool_name": "Bash"]) { d, _ in body = d }
        drainMain()
        XCTAssertEqual(body, HookResponse.permission(event: "PermissionRequest", allow: false, message: "Beacon device offline"))
        XCTAssertEqual(p.heldCountForTest(), 0)
        XCTAssertTrue(sink.raises.isEmpty)
    }

    // A deliverable prompt is HELD (no response yet) and raised to the mux; resolving it fulfills the
    // connection with the Codex-compatible allow decision and ends the prompt.
    func testHeldPromptRaisesThenResolves() {
        let (p, sink) = makeProvider()
        var body: Data?
        p.handlePermissionForTest(body: ["hook_event_name": "PermissionRequest", "session_id": "s1",
                                         "tool_name": "Bash", "tool_input": ["command": "ls"]]) { d, _ in body = d }
        drainMain()
        XCTAssertNil(body, "a held prompt must not respond until resolved")
        XCTAssertEqual(p.heldCountForTest(), 1)
        XCTAssertEqual(sink.raises.count, 1)
        XCTAssertEqual(sink.raises.first?.sessionKey, "s1")

        guard let nid = p.lastNativeIdForTest() else { return XCTFail("no native id") }
        XCTAssertEqual(p.resolvePrompt(nativeID: nid, approve: true), .applied)
        drainMain()
        XCTAssertEqual(body, HookResponse.permission(event: "PermissionRequest", allow: true))
        XCTAssertEqual(p.heldCountForTest(), 0)
        XCTAssertEqual(sink.ends, [nid])
    }

    // The 575s cap denies a held prompt (fail-closed), mirroring ClaudeCodeProvider.
    func testCapDeniesHeldPrompt() {
        let (p, _) = makeProvider()
        var body: Data?
        p.handlePermissionForTest(body: ["hook_event_name": "PermissionRequest", "tool_name": "Bash"]) { d, _ in body = d }
        drainMain()
        guard let nid = p.lastNativeIdForTest() else { return XCTFail("no native id") }
        p.expirePromptForTest(nativeID: nid)
        drainMain()
        XCTAssertEqual(body, HookResponse.permission(event: "PermissionRequest", allow: false))
        XCTAssertEqual(p.heldCountForTest(), 0)
    }

    // Buddy toggled OFF while a prompt is held releases it pass-through (no verdict), not a deny.
    func testBuddyToggleOffReleasesHeldPassthrough() {
        let (p, _) = makeProvider()
        var body: Data?
        p.handlePermissionForTest(body: ["hook_event_name": "PermissionRequest", "tool_name": "Bash"]) { d, _ in body = d }
        drainMain()
        XCTAssertEqual(p.heldCountForTest(), 1)
        p.setEnabled(EnabledCapabilities(usage: true, buddy: false))
        XCTAssertEqual(p.heldCountForTest(), 0)   // queue.sync barrier: the release ran on the ingest queue
        drainMain()
        XCTAssertEqual(body, Data("{}".utf8))
    }

    // Quit drain denies every held prompt (fail-closed) and clears the queue.
    func testDrainDeniesEveryHeldPrompt() {
        let (p, _) = makeProvider()
        p.injectPermissionForTest(sessionId: "s1", tool: "Bash", hint: "ls")
        p.injectPermissionForTest(sessionId: "s2", tool: "Bash", hint: "cat")
        XCTAssertEqual(p.heldCountForTest(), 2)
        let done = expectation(description: "drained")
        p.drainHeldPrompts(reason: "quitting") { done.fulfill() }
        wait(for: [done], timeout: 2)
        XCTAssertEqual(p.heldCountForTest(), 0)
    }
}
