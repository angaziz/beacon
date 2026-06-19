import XCTest
import BeaconHubKit
@testable import beacon_hub

final class ClaudeCodeBridgeTests: XCTestCase {

    // #93: a rate_limits-carrying statusline POST proves Claude Code is alive regardless of whether the
    // parsed value changed, so the liveness hook must fire on EVERY such POST while onClaudeUsage stays
    // deduped (#59). Two byte-identical payloads => activity x2, usage x1.
    func testStatuslineActivityFiresEveryPostWhileUsageDedups() {
        let bridge = ClaudeCodeBridge()
        var activityCount = 0
        var usageCount = 0
        bridge.onStatuslineActivity = { activityCount += 1 }
        bridge.onClaudeUsage = { _ in usageCount += 1 }

        let body: [String: Any] = [
            "session_id": "s1",
            "rate_limits": [
                "five_hour": ["used_percentage": 12, "resets_at": 1_700_000_000],
                "seven_day": ["used_percentage": 34, "resets_at": 1_700_500_000],
            ],
        ]
        bridge.handleStatusline(body)
        bridge.handleStatusline(body)

        // Callbacks hop to the main queue; drain it (FIFO) before asserting.
        let drained = expectation(description: "main queue drained")
        DispatchQueue.main.async { drained.fulfill() }
        wait(for: [drained], timeout: 1)

        XCTAssertEqual(activityCount, 2, "liveness must fire on every rate_limits POST")
        XCTAssertEqual(usageCount, 1, "unchanged value must stay deduped (#59)")
    }

    func testSecondPromptQueuesInsteadOfDenying() {
        let bridge = ClaudeCodeBridge()
        bridge.setDeviceConnected(true)
        var published: [BuddyState] = []
        bridge.onBuddyUpdate = { published.append($0) }

        bridge.injectPermissionForTest(toolUseId: "t1", tool: "Bash", hint: "one")
        bridge.injectPermissionForTest(toolUseId: "t2", tool: "Write", hint: "two")

        drainMain()   // onBuddyUpdate hops to the main queue (publishBuddy); flush it before asserting.
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

        drainMain()
        let frontId = published.last!.prompt!.id
        _ = bridge.resolve(id: frontId, approve: true)

        drainMain()
        let next = published.last!.prompt!
        XCTAssertEqual(next.hint, "two", "front advanced to the second prompt")
        XCTAssertNil(next.qlen, "only one left => qlen omitted")
    }

    // onBuddyUpdate is delivered via DispatchQueue.main.async; flush pending main-queue work (FIFO)
    // so the published snapshots are visible before assertions.
    private func drainMain() {
        let drained = expectation(description: "main queue drained")
        DispatchQueue.main.async { drained.fulfill() }
        wait(for: [drained], timeout: 1)
    }
}
