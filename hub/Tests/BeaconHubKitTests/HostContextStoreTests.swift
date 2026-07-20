import XCTest
@testable import BeaconHubKit

final class HostContextStoreTests: XCTestCase {
    func testSetAndLookup() {
        let s = HostContextStore()
        s.set(key: "cc-1", app: "WarpTerminal", focusURL: "warp://focus/123",
              bundleId: "dev.warp.Warp-Stable", cwd: "/home/user/beacon")
        let h = s.host(for: "cc-1")
        XCTAssertEqual(h?.app, "WarpTerminal")
        XCTAssertEqual(h?.focusURL, "warp://focus/123")
        XCTAssertEqual(h?.bundleId, "dev.warp.Warp-Stable")
        XCTAssertEqual(h?.cwd, "/home/user/beacon")
    }

    func testUnknownReturnsNil() {
        XCTAssertNil(HostContextStore().host(for: "ghost"))
    }

    func testEmptyStringsLeaveFieldsNil() {
        let s = HostContextStore()
        s.set(key: "b", app: "", focusURL: "", bundleId: "", cwd: "/tmp/x")
        let h = s.host(for: "b")
        XCTAssertNil(h?.app); XCTAssertNil(h?.focusURL); XCTAssertNil(h?.bundleId)
        XCTAssertEqual(h?.cwd, "/tmp/x")
    }

    // I2: a later set with empty/nil fields (a `compact` event that didn't re-export WARP_FOCUS_URL)
    // must MERGE, not wipe a previously-captured precise handle.
    func testMergePreservesPriorValues() {
        let s = HostContextStore()
        s.set(key: "m", app: "WarpTerminal", focusURL: "warp://focus/abc", bundleId: "dev.warp.Warp-Stable", cwd: "/x/api")
        s.set(key: "m", app: "WarpTerminal", focusURL: "", bundleId: nil, cwd: nil)   // partial re-send
        var h = s.host(for: "m")
        XCTAssertEqual(h?.focusURL, "warp://focus/abc")        // NOT wiped by empty string
        XCTAssertEqual(h?.bundleId, "dev.warp.Warp-Stable")     // NOT wiped by nil
        s.set(key: "m", app: nil, focusURL: "warp://focus/xyz", bundleId: nil, cwd: nil)   // non-empty overwrites
        h = s.host(for: "m")
        XCTAssertEqual(h?.focusURL, "warp://focus/xyz")
        XCTAssertEqual(h?.app, "WarpTerminal")                  // untouched by nil
    }

    func testRemove() {
        let s = HostContextStore()
        s.set(key: "x", app: "T", focusURL: nil, bundleId: nil, cwd: "/x")
        s.remove(key: "x")
        XCTAssertNil(s.host(for: "x"))
    }
}
