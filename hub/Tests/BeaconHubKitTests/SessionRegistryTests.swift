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

    func testMarkNeedsInputProducesQuestion() {
        let r = SessionRegistry(idleTTL: 300)
        r.touchActivity(sessionId: "A", cwd: "/x/api", now: t0)
        r.markNeedsInput(sessionId: "A")
        let snap = r.snapshot(now: t0.addingTimeInterval(1), waitingFront: nil, waitingQueued: [])
        XCTAssertEqual(snap.first?.state, .question)
    }

    func testActivityClearsQuestion() {
        let r = SessionRegistry(idleTTL: 300)
        r.touchActivity(sessionId: "A", cwd: "/x/api", now: t0)
        r.markNeedsInput(sessionId: "A")
        r.touchActivity(sessionId: "A", cwd: "/x/api", now: t0.addingTimeInterval(1))
        let snap = r.snapshot(now: t0.addingTimeInterval(2), waitingFront: nil, waitingQueued: [])
        XCTAssertEqual(snap.first?.state, .working)
    }

    func testQuestionWinsOverStaleAndStopped() {
        struct C { let name: String; let setup: (SessionRegistry) -> Void; let expect: SessionState }
        let cases: [C] = [
            // needsInput + stale => still question (Claude is waiting regardless of age)
            C(name: "question beats stale", setup: {
                $0.touchActivity(sessionId: "A", cwd: "/x", now: self.t0)
                $0.markNeedsInput(sessionId: "A") }, expect: .question),
            // needsInput + stopped => question (not attention)
            C(name: "question beats stopped", setup: {
                $0.touchActivity(sessionId: "A", cwd: "/x", now: self.t0)
                $0.markStop(sessionId: "A", now: self.t0)
                $0.markNeedsInput(sessionId: "A") }, expect: .question),
        ]
        for c in cases {
            let r = SessionRegistry(idleTTL: 300)
            c.setup(r)
            // Query 400s later so the session is stale.
            let snap = r.snapshot(now: t0.addingTimeInterval(400), waitingFront: nil, waitingQueued: [])
            XCTAssertEqual(snap.first?.state, c.expect, c.name)
        }
    }

    func testWaitingBeatsQuestion() {
        let r = SessionRegistry(idleTTL: 300)
        r.touchActivity(sessionId: "A", cwd: "/x/api", now: t0)
        r.markNeedsInput(sessionId: "A")
        // front-prompt session overrides question
        let snap = r.snapshot(now: t0.addingTimeInterval(1), waitingFront: "A", waitingQueued: [])
        XCTAssertEqual(snap.first?.state, .waiting)
    }

    func testProviderIdAgentRouteAndTrackedCount() {
        let r = SessionRegistry()
        r.touchActivity(providerID: "claude", sessionId: "cc-1", cwd: "/x/api", now: t0)
        r.touchActivity(providerID: "codex", sessionId: "cx-1", cwd: "/x/srv", now: t0.addingTimeInterval(1))
        let snap = r.snapshot(now: t0.addingTimeInterval(2), waitingFront: nil, waitingQueued: [])
        XCTAssertEqual(Set(snap.compactMap { $0.agent }), ["claude", "codex"])   // agent = providerID
        // route: the codex row's short id resolves back to (codex, cx-1).
        let codexRow = snap.first { $0.agent == "codex" }!
        let route = r.route(shortId: codexRow.id)
        XCTAssertEqual(route?.providerID, "codex")
        XCTAssertEqual(route?.nativeKey, "cx-1")
        // trackedCount scoped to enabled providers.
        XCTAssertEqual(r.trackedCount(), 2)
        XCTAssertEqual(r.trackedCount(includeProvider: { $0 == "claude" }), 1)
    }
}
