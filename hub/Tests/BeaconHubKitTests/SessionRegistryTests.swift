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

    func testSetHostAndHostByShortId() {
        let r = SessionRegistry()
        r.touchActivity(sessionId: "CC-abc", cwd: "/home/user/beacon", now: t0)
        r.setHost(sessionId: "CC-abc", hostApp: "WarpTerminal", focusURL: "warp://focus/123", bundleId: "dev.warp.Warp-Stable")

        // host(for:) returns populated fields for the full CC session_id.
        let byFull = r.host(for: "CC-abc")
        XCTAssertNotNil(byFull)
        XCTAssertEqual(byFull?.app, "WarpTerminal")
        XCTAssertEqual(byFull?.focusURL, "warp://focus/123")
        XCTAssertEqual(byFull?.bundleId, "dev.warp.Warp-Stable")
        XCTAssertEqual(byFull?.cwd, "/home/user/beacon")

        // hostByShortId finds the same entry via the minted s<n> id.
        let snap = r.snapshot(now: t0.addingTimeInterval(1), waitingFront: nil, waitingQueued: [])
        let shortId = snap.first!.id   // e.g. "s1"
        let byShort = r.hostByShortId(shortId)
        XCTAssertNotNil(byShort)
        XCTAssertEqual(byShort?.app, "WarpTerminal")
        XCTAssertEqual(byShort?.focusURL, "warp://focus/123")
        XCTAssertEqual(byShort?.cwd, "/home/user/beacon")
    }

    func testHostUnknownReturnsNil() {
        let r = SessionRegistry()
        XCTAssertNil(r.host(for: "no-such-session"))
        XCTAssertNil(r.hostByShortId("s99"))
    }

    func testSetHostNoOpForUnknownSession() {
        let r = SessionRegistry()
        // Should not crash; there is simply no entry to update.
        r.setHost(sessionId: "ghost", hostApp: "Terminal", focusURL: nil, bundleId: nil)
        XCTAssertNil(r.host(for: "ghost"))
    }

    func testHostEmptyStringsLeaveFieldsNil() {
        let r = SessionRegistry()
        r.touchActivity(sessionId: "B", cwd: "/tmp/x", now: t0)
        r.setHost(sessionId: "B", hostApp: "", focusURL: "", bundleId: "")
        let h = r.host(for: "B")
        XCTAssertNil(h?.app)
        XCTAssertNil(h?.focusURL)
        XCTAssertNil(h?.bundleId)
        XCTAssertEqual(h?.cwd, "/tmp/x")   // cwd from touchActivity is kept
    }

    // I2: a later setHost with empty/nil fields (e.g. a `compact` event that didn't re-export
    // WARP_FOCUS_URL) must MERGE, not wipe a previously-captured precise handle.
    func testSetHostMergesAndPreservesPriorValues() {
        let r = SessionRegistry()
        r.touchActivity(sessionId: "M", cwd: "/x/api", now: t0)
        r.setHost(sessionId: "M", hostApp: "WarpTerminal", focusURL: "warp://focus/abc", bundleId: "dev.warp.Warp-Stable")

        // Second call: empty focus_url + nil bundle, only host_app re-sent -> prior values survive.
        r.setHost(sessionId: "M", hostApp: "WarpTerminal", focusURL: "", bundleId: nil)
        let h = r.host(for: "M")
        XCTAssertEqual(h?.app, "WarpTerminal")
        XCTAssertEqual(h?.focusURL, "warp://focus/abc")        // NOT wiped by empty string
        XCTAssertEqual(h?.bundleId, "dev.warp.Warp-Stable")     // NOT wiped by nil

        // A non-empty incoming value still overwrites.
        r.setHost(sessionId: "M", hostApp: nil, focusURL: "warp://focus/xyz", bundleId: nil)
        XCTAssertEqual(r.host(for: "M")?.focusURL, "warp://focus/xyz")
        XCTAssertEqual(r.host(for: "M")?.app, "WarpTerminal")   // untouched by nil
    }
}
