import XCTest
import BeaconHubKit
@testable import beacon_hub

final class PiProviderTests: XCTestCase {

    private final class MockSink: ProviderSink {
        var didRaisePrompt = false
        var sessions: [ProviderSessionEvent] = []
        func provider(_ id: String, didUpdateUsage usage: ProviderUsage) {}
        func provider(_ id: String, didUpdateMetrics tokens: Int, contextPct: Int) {}
        func provider(_ id: String, didUpdateSession event: ProviderSessionEvent) { sessions.append(event) }
        func provider(_ id: String, didRaisePrompt nativeID: String, tool: String, hint: String, sessionNativeKey: String?) {
            didRaisePrompt = true
        }
        func provider(_ id: String, didEndPrompt nativeID: String) {}
        func provider(_ id: String, didAppendEntry line: String) {}
    }

    private func makeProvider() -> (HookBuddyProvider, MockSink) {
        let provider = HookBuddyProvider(
            descriptor: ProviderDescriptor(id: "pi", label: "PI", capabilities: [.sessions, .prompts]),
            routePath: PiHooks.routePath,
            capSeconds: 26,
            server: LocalIngestServer(),
            permissionRoute: "/pi/permission",
            passThroughOperationalFailures: true)
        let sink = MockSink()
        provider.branchResolverForTest = { _ in nil }
        provider.start(sink: sink)
        provider.setDeviceConnected(true)
        return (provider, sink)
    }

    private func drainMain() {
        let expectation = expectation(description: "main")
        DispatchQueue.main.async { expectation.fulfill() }
        wait(for: [expectation], timeout: 1)
    }

    func testLifecycleEventsMapToSessionStates() {
        let cases: [(event: String, expects: (ProviderSessionEvent) -> Bool)] = [
            ("SessionStart", { if case .activity(let key, _) = $0 { return key == "s1" } else { return false } }),
            ("UserPromptSubmit", { if case .activity(let key, _) = $0 { return key == "s1" } else { return false } }),
            ("Stop", { if case .stop(let key, _) = $0 { return key == "s1" } else { return false } }),
            ("SessionEnd", { if case .end(let key) = $0 { return key == "s1" } else { return false } })
        ]
        for testCase in cases {
            let (provider, sink) = makeProvider()
            XCTAssertTrue(provider.applySessionHookForTest(event: testCase.event, sessionId: "s1", cwd: "/tmp/proj"))
            drainMain()
            XCTAssertTrue(sink.sessions.first.map(testCase.expects) ?? false,
                          "\(testCase.event): wrong mapping \(String(describing: sink.sessions.first))")
        }
    }

    func testUnknownHookEventIsNotRouted() {
        let (provider, sink) = makeProvider()
        for event in ["Notification", "agent_settled", "", "ApprovalResolved"] {
            XCTAssertFalse(provider.applySessionHookForTest(event: event, sessionId: "s1", cwd: "/tmp/proj"))
        }
        drainMain()
        XCTAssertTrue(sink.sessions.isEmpty, "unknown events must emit nothing")
    }

    func testPiOperationalFailuresUsePiWireShapes() {
        let cases: [(name: String, arrange: (HookBuddyProvider) -> Void, expected: Data)] = [
            ("buddy off", { $0.setEnabled(EnabledCapabilities(usage: true, buddy: false)) }, Data("{\"unavailable\":true}".utf8)),
            ("device offline", { $0.setDeviceConnected(false) }, Data("{\"device\":false}".utf8))
        ]
        for testCase in cases {
            let (provider, sink) = makeProvider()
            testCase.arrange(provider)
            var body: Data?
            provider.handlePermissionForTest(body: ["hook_event_name": "PermissionRequest", "tool_name": "bash", "session_id": "s1"]) { data, _ in body = data }
            drainMain()
            XCTAssertEqual(body, testCase.expected, "\(testCase.name): must use Pi's explicit pass-through wire shape")
            XCTAssertEqual(provider.heldCountForTest(), 0)
            XCTAssertFalse(sink.didRaisePrompt)
        }
    }

    func testPiReleaseLeavesLateTombstoneForDeviceDecision() {
        let (provider, _) = makeProvider()
        provider.injectPermissionForTest(sessionId: "s1", tool: "Bash", hint: "rm -rf /tmp")
        drainMain()
        guard let id = provider.lastNativeIdForTest() else { return XCTFail("expected held prompt") }
        provider.setEnabled(EnabledCapabilities(usage: true, buddy: false))
        var outcome: ResolveOutcome?
        provider.resolvePrompt(nativeID: id, approve: true) { outcome = $0 }
        XCTAssertEqual(outcome, .late, "late device decision after Pi release must ack false, not unknown")
    }

    func testPiQuitReleasesWhileCodexStillDenies() {
        let (pi, _) = makeProvider()
        pi.injectPermissionForTest(sessionId: "s1", tool: "Bash", hint: "rm -rf /tmp")
        let codex = HookBuddyProvider(descriptor: ProviderDescriptor(id: "codex", label: "Codex", capabilities: [.sessions, .prompts]), routePath: "/codex/hook", capSeconds: 26, server: LocalIngestServer())
        let sink = MockSink(); codex.start(sink: sink); codex.setDeviceConnected(true)
        var codexResponse: Data?
        codex.handlePermissionForTest(body: ["tool_name": "bash", "session_id": "s1"]) { data, onSent in codexResponse = data; onSent?() }
        let done = expectation(description: "drained")
        pi.drainHeldPrompts(reason: "quit") { done.fulfill() }
        wait(for: [done], timeout: 1)
        XCTAssertEqual(pi.heldCountForTest(), 0)
        XCTAssertEqual(codexResponse, nil, "Codex remains held until its own fail-closed drain")
        let codexDone = expectation(description: "codex drained")
        codex.drainHeldPrompts(reason: "quit") { codexDone.fulfill() }
        wait(for: [codexDone], timeout: 1)
        XCTAssertNotEqual(codexResponse, Data("{}".utf8), "Codex quit remains deny")
    }

    func testDeferredAckWaitsForPiCommit() {
        let provider = HookBuddyProvider(
            descriptor: ProviderDescriptor(id: "pi", label: "PI", capabilities: [.sessions, .prompts]),
            routePath: PiHooks.routePath,
            capSeconds: 26,
            server: LocalIngestServer(),
            permissionRoute: "/pi/permission",
            passThroughOperationalFailures: true)
        let sink = MockSink()
        provider.start(sink: sink)
        provider.setDeviceConnected(true)
        provider.injectPermissionForTest(sessionId: "s1", tool: "Bash", hint: "rm -rf /tmp")
        drainMain()
        guard let nativeID = provider.lastNativeIdForTest() else {
            return XCTFail("injected prompt must mint an id")
        }
        var outcome: ResolveOutcome?
        provider.resolvePrompt(nativeID: nativeID, approve: true) { outcome = $0 }
        XCTAssertNil(outcome, "device ack must wait for the extension commit")
        provider.commitPiForTest(nativeID: nativeID, applied: true)
        XCTAssertEqual(outcome, .applied)

        provider.injectPermissionForTest(sessionId: "s1", tool: "Bash", hint: "rm -rf /tmp")
        drainMain()
        guard let lostRaceID = provider.lastNativeIdForTest() else {
            return XCTFail("second injected prompt must mint an id")
        }
        provider.resolvePrompt(nativeID: lostRaceID, approve: false) { outcome = $0 }
        provider.commitPiForTest(nativeID: lostRaceID, applied: false)
        XCTAssertEqual(outcome, .late, "lost local-TUI race must become device ok:false")
    }

    func testDeferredAckTimesOutAsLateWithInjectableWindow() {
        let provider = HookBuddyProvider(
            descriptor: ProviderDescriptor(id: "pi", label: "PI", capabilities: [.sessions, .prompts]),
            routePath: PiHooks.routePath, capSeconds: 26, server: LocalIngestServer(),
            permissionRoute: "/pi/permission", passThroughOperationalFailures: true, piCommitSeconds: 0.01)
        let sink = MockSink(); provider.start(sink: sink); provider.setDeviceConnected(true)
        provider.injectPermissionForTest(sessionId: "s1", tool: "Bash", hint: "rm -rf /tmp")
        guard let id = provider.lastNativeIdForTest() else { return XCTFail("expected prompt") }
        let done = expectation(description: "commit cap")
        var outcome: ResolveOutcome?
        provider.resolvePrompt(nativeID: id, approve: true) { value in outcome = value; done.fulfill() }
        wait(for: [done], timeout: 1)
        XCTAssertEqual(outcome, .late)
    }

    func testDescriptorKeepsBuddyToggleWithoutPrompts() {
        let descriptor = ProviderDescriptor(id: "pi", label: "PI", capabilities: [.sessions])
        XCTAssertTrue(descriptor.supportsBuddy, "sessions alone must still offer the buddy toggle")
        XCTAssertFalse(descriptor.supportsUsage)
        XCTAssertFalse(descriptor.capabilities.contains(.prompts))
    }
}
