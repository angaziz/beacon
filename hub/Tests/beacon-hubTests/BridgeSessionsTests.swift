import XCTest
import BeaconHubKit
@testable import beacon_hub

final class BridgeSessionsTests: XCTestCase {
    // onSessionsUpdate hops to DispatchQueue.main.async (like onBuddyUpdate); pump main before asserting.
    private func drainMain() {
        let e = expectation(description: "main"); DispatchQueue.main.async { e.fulfill() }
        wait(for: [e], timeout: 1)
    }

    // A statusline render after a turn ends must NOT resurrect the session to .working: Claude Code
    // emits a final statusline at/after Stop, and treating it as work froze finished sessions on
    // "Working" (the bug this guards). Real work signals (UserPromptSubmit, SessionStart, permission)
    // clear attention -- statusline only keeps the session alive + refreshes stats.
    func testStatuslineDoesNotResurrectStoppedSession() {
        let b = ClaudeCodeBridge()
        var last: [Session] = []
        b.onSessionsUpdate = { last = $0 }
        b.applySessionHookForTest(event: "SessionStart", sessionId: "A", cwd: "/x/api")
        b.applySessionHookForTest(event: "Stop", sessionId: "A", cwd: "/x/api")
        drainMain()
        XCTAssertEqual(last.first(where: { $0.label.hasPrefix("api") })?.state, .attention)
        b.handleStatusline(["session_id": "A", "context_window": ["used_percentage": 10]])   // already internal
        drainMain()
        XCTAssertEqual(last.first(where: { $0.label.hasPrefix("api") })?.state, .attention)   // still done, not working
    }

    // UserPromptSubmit must re-arm .working the instant the next turn starts, without relying on a
    // statusline POST (which CC's own refresh cadence -- outside Beacon's control -- may delay).
    func testUserPromptSubmitProducesWorkingAfterStop() {
        let b = ClaudeCodeBridge()
        var last: [Session] = []
        b.onSessionsUpdate = { last = $0 }
        b.applySessionHookForTest(event: "SessionStart", sessionId: "A", cwd: "/x/api")
        b.applySessionHookForTest(event: "Stop", sessionId: "A", cwd: "/x/api")
        drainMain()
        XCTAssertEqual(last.first(where: { $0.label.hasPrefix("api") })?.state, .attention)
        b.applySessionHookForTest(event: "UserPromptSubmit", sessionId: "A", cwd: "/x/api")
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
        XCTAssertNotNil(last.first)   // session must be present (guards the next assertion from being vacuous)
        XCTAssertNotEqual(last.first?.state, .waiting)   // Notification is not a held prompt
    }

    func testNotificationAsFirstSignalCreatesQuestionSession() {
        // Regression for I1/M4: if Notification arrives before SessionStart the entry must still
        // be created (touchActivity) and marked .question (markNeedsInput). The old code called
        // markNeedsInput on a missing entry, which silently no-oped and dropped the question.
        let b = ClaudeCodeBridge()
        var last: [Session] = []
        b.onSessionsUpdate = { last = $0 }
        b.applySessionHookForTest(event: "Notification", sessionId: "B", cwd: "/x/srv")
        drainMain()
        XCTAssertNotNil(last.first, "Notification as first signal must create a session entry")
        XCTAssertEqual(last.first?.state, .question)
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

    func testQuestionFiresItsOwnBucketTransition() {
        // Regression: spec §6 folded questions into `attention`, but #110 split .question out as a
        // state ranked above .attention -- an asking session stopped reaching the attention branch and
        // lost its chime entirely. .question now has its own bucket edge, on the same aggregate rule.
        let b = ClaudeCodeBridge()
        var questions = 0
        var attentions = 0
        b.onQuestion = { questions += 1 }
        b.onAttention = { attentions += 1 }
        b.applySessionHookForTest(event: "SessionStart", sessionId: "A", cwd: "/x/api")
        b.applySessionHookForTest(event: "SessionStart", sessionId: "B", cwd: "/x/api")
        b.applySessionHookForTest(event: "Notification", sessionId: "A", cwd: "/x/api")  // 0 -> >0: fire
        b.applySessionHookForTest(event: "Notification", sessionId: "B", cwd: "/x/api")  // already >0: silent
        drainMain()
        XCTAssertEqual(questions, 1)
        XCTAssertEqual(attentions, 0, "a question must not also fire the turn-finished chime")
    }

    func testNotificationProducesQuestion() {
        let b = ClaudeCodeBridge()
        var last: [Session] = []
        b.onSessionsUpdate = { last = $0 }
        b.applySessionHookForTest(event: "SessionStart", sessionId: "A", cwd: "/x/api")
        b.applySessionHookForTest(event: "Notification", sessionId: "A", cwd: "/x/api")
        drainMain()
        XCTAssertEqual(last.first(where: { $0.label.hasPrefix("api") })?.state, .question)
    }

    // A statusline render must NOT clear a pending question (same reasoning as .attention): the
    // question stands until the user actually responds, which fires UserPromptSubmit. Statusline is
    // just a UI render firing ~3x/s and would otherwise wipe the "asking a question" indicator
    // almost immediately.
    func testStatuslineDoesNotClearQuestionButUserPromptDoes() {
        let b = ClaudeCodeBridge()
        var last: [Session] = []
        b.onSessionsUpdate = { last = $0 }
        b.applySessionHookForTest(event: "SessionStart", sessionId: "A", cwd: "/x/api")
        b.applySessionHookForTest(event: "Notification", sessionId: "A", cwd: "/x/api")
        drainMain()
        XCTAssertEqual(last.first(where: { $0.label.hasPrefix("api") })?.state, .question)
        b.handleStatusline(["session_id": "A", "context_window": ["used_percentage": 20]])
        drainMain()
        XCTAssertEqual(last.first(where: { $0.label.hasPrefix("api") })?.state, .question)   // still asking
        b.applySessionHookForTest(event: "UserPromptSubmit", sessionId: "A", cwd: "/x/api")   // user answered
        drainMain()
        XCTAssertEqual(last.first(where: { $0.label.hasPrefix("api") })?.state, .working)
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
