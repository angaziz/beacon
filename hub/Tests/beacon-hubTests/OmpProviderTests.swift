import XCTest
import BeaconHubKit
@testable import beacon_hub

// omp buddy adapter (beacon-omp v4): the omp provider mirrors approval waits instead of gating them.
// omp settles tool approval before an extension can answer it, so this descriptor drops .prompts and
// the adapter must never hold an omp tool call. Session-state aggregation itself lives in ProviderMux.
final class OmpProviderTests: XCTestCase {

    private final class MockSink: ProviderSink {
        var raises: [(nativeID: String, tool: String, hint: String, sessionKey: String?)] = []
        var sessions: [ProviderSessionEvent] = []
        func provider(_ id: String, didUpdateUsage usage: ProviderUsage) {}
        func provider(_ id: String, didUpdateMetrics tokens: Int, contextPct: Int) {}
        func provider(_ id: String, didUpdateSession event: ProviderSessionEvent) { sessions.append(event) }
        func provider(_ id: String, didRaisePrompt nativeID: String, tool: String, hint: String, sessionNativeKey: String?) {
            raises.append((nativeID, tool, hint, sessionNativeKey))
        }
        func provider(_ id: String, didEndPrompt nativeID: String) {}
        func provider(_ id: String, didAppendEntry line: String) {}
    }

    // Mirrors the app's registration in AppDelegate: sessions plane only, no .prompts.
    private func makeProvider() -> (HookBuddyProvider, MockSink) {
        let p = HookBuddyProvider(
            descriptor: ProviderDescriptor(id: "omp", label: "OMP", capabilities: [.sessions]),
            routePath: OmpHooks.routePath, capSeconds: 26, server: LocalIngestServer())
        let sink = MockSink()
        p.branchResolverForTest = { _ in nil }   // never shell git in tests
        p.start(sink: sink)
        p.setDeviceConnected(true)
        return (p, sink)
    }

    private func drainMain() {
        let e = expectation(description: "main"); DispatchQueue.main.async { e.fulfill() }
        wait(for: [e], timeout: 1)
    }

    // The v4 approval mirror: tool_approval_requested => Notification => question (the Mac is asking),
    // tool_approval_resolved => ApprovalResolved => activity (answered, session back to working).
    func testApprovalMirrorEventsMapToSessionStates() {
        let cases: [(event: String, expect: (ProviderSessionEvent) -> Bool)] = [
            ("Notification",     { if case .needsInput(let k, _) = $0 { return k == "s1" } else { return false } }),
            ("ApprovalResolved", { if case .activity(let k, _) = $0 { return k == "s1" } else { return false } }),
        ]
        for c in cases {
            let (p, sink) = makeProvider()
            XCTAssertTrue(p.applySessionHookForTest(event: c.event, sessionId: "s1", cwd: "/tmp/proj"),
                          "\(c.event): must be a routed hook event, not silently dropped")
            drainMain()
            let first = sink.sessions.first
            XCTAssertNotNil(first, "\(c.event): no session event emitted")
            XCTAssertTrue(first.map(c.expect) ?? false, "\(c.event): wrong mapping (\(String(describing: first)))")
        }
    }

    // SessionHookKind is the routed-event allowlist, so an event omp never sends must not resolve --
    // otherwise an unknown hook body would land in the activity branch and fake liveness.
    func testUnknownHookEventIsNotRouted() {
        let (p, sink) = makeProvider()
        for event in ["PreToolUse", "", "notification", "ApprovalRequested"] {
            XCTAssertFalse(p.applySessionHookForTest(event: event, sessionId: "s1", cwd: "/tmp/proj"),
                           "\(event) must not resolve to a session transition")
        }
        drainMain()
        XCTAssertTrue(sink.sessions.isEmpty, "unknown events must emit nothing")
    }

    // A provider without .prompts never holds a tool call. A stale v3 extension still POSTing
    // PermissionRequest reads {} as passthrough, so it stops gating at once instead of at reinstall.
    // The guard precedes buddy-off, offline, and quit-drain handling: there is nothing to fail closed.
    func testPermissionRequestAlwaysPassesThroughWithoutPrompts() {
        let cases: [(name: String, arrange: (HookBuddyProvider) -> Void)] = [
            ("buddy on, device online", { _ in }),
            ("buddy off", { $0.setEnabled(EnabledCapabilities(usage: true, buddy: false)) }),
            ("device offline", { $0.setDeviceConnected(false) }),
        ]
        for c in cases {
            let (p, sink) = makeProvider()
            c.arrange(p)
            var body: Data?
            p.handlePermissionForTest(body: ["hook_event_name": "PermissionRequest", "tool_name": "bash",
                                             "session_id": "s1"]) { d, _ in body = d }
            drainMain()
            XCTAssertEqual(body, Data("{}".utf8), "\(c.name): must be a no-verdict passthrough")
            XCTAssertEqual(p.heldCountForTest(), 0, "\(c.name): nothing may be held")
            XCTAssertTrue(sink.raises.isEmpty, "\(c.name): no device prompt may be raised")
        }
    }

    // Quit drain must not turn a non-gating provider's passthrough into a fail-closed deny: the omp
    // tool call was never held, so denying it would fail a command the user never saw on the device.
    func testPermissionRequestPassesThroughWhileQuitting() {
        let (p, _) = makeProvider()
        let drained = expectation(description: "drained")
        p.drainHeldPrompts(reason: "Beacon hub is quitting") { drained.fulfill() }
        wait(for: [drained], timeout: 1)

        var body: Data?
        p.handlePermissionForTest(body: ["hook_event_name": "PermissionRequest", "tool_name": "bash"]) { d, _ in body = d }
        drainMain()
        XCTAssertEqual(body, Data("{}".utf8), "quitting must still pass through, never deny")
    }

    // The descriptor still offers the Coding Buddy toggle (buddy = sessions OR prompts), so the omp
    // card keeps its switch even though remote approve/deny is gone.
    func testDescriptorKeepsBuddyToggleWithoutPrompts() {
        let d = ProviderDescriptor(id: "omp", label: "OMP", capabilities: [.sessions])
        XCTAssertTrue(d.supportsBuddy, "sessions alone must still offer the buddy toggle")
        XCTAssertFalse(d.supportsUsage)
        XCTAssertFalse(d.capabilities.contains(.prompts))
    }
}
