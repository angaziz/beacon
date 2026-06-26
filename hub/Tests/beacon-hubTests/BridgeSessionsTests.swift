import XCTest
import BeaconHubKit
@testable import beacon_hub

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

    func testBranchResolvedAndCachedPerCwd() {
        let b = ClaudeCodeBridge()
        var resolves = 0
        // Non-default branch so it survives label()'s redundant-branch drop ("main"/"master").
        b.branchResolverForTest = { _ in resolves += 1; return "feat/x" }   // injected resolver
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
}
