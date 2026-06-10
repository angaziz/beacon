import XCTest
@testable import BeaconHubKit

final class BurnPaceTests: XCTestCase {
    // "Show only after >= 3 samples of movement" (issue #57). The baseline (first) sample is not a
    // movement, so 3 movements require 4 updates: nil through the 3rd, a pace on the 4th.
    func testNoPaceUntilThreeMovementSamples() {
        var e = BurnPaceEstimator()
        XCTAssertNil(e.update(pct: 10, reset: 1000, now: 0).pace)   // baseline, 0 movements
        XCTAssertNil(e.update(pct: 12, reset: 1000, now: 60).pace)  // movement 1
        XCTAssertNil(e.update(pct: 14, reset: 1000, now: 120).pace) // movement 2
        XCTAssertNotNil(e.update(pct: 16, reset: 1000, now: 180).pace) // movement 3 => pace
    }

    func testPaceIsRoughlyPercentPerHour() {
        var e = BurnPaceEstimator()
        _ = e.update(pct: 10, reset: 100000, now: 0)
        _ = e.update(pct: 12, reset: 100000, now: 60)
        _ = e.update(pct: 14, reset: 100000, now: 120)
        let r = e.update(pct: 16, reset: 100000, now: 180)
        XCTAssertEqual(r.pace!, 120, accuracy: 30)
    }

    func testResetOnPctDropTenPoints() {
        var e = BurnPaceEstimator()
        _ = e.update(pct: 50, reset: 100000, now: 0)
        _ = e.update(pct: 52, reset: 100000, now: 60)
        _ = e.update(pct: 54, reset: 100000, now: 120)
        let afterReset = e.update(pct: 5, reset: 200000, now: 180)
        XCTAssertNil(afterReset.pace)
    }

    func testResetOnSampleGapOverTenMinutes() {
        var e = BurnPaceEstimator()
        _ = e.update(pct: 10, reset: 100000, now: 0)
        _ = e.update(pct: 12, reset: 100000, now: 60)
        _ = e.update(pct: 14, reset: 100000, now: 120)
        let afterGap = e.update(pct: 16, reset: 100000, now: 120 + 11*60)
        XCTAssertNil(afterGap.pace)
    }

    func testEtaAndCapVsResetStyling() {
        var e = BurnPaceEstimator()
        _ = e.update(pct: 80, reset: 999_999_999, now: 0)
        _ = e.update(pct: 84, reset: 999_999_999, now: 60)
        _ = e.update(pct: 88, reset: 999_999_999, now: 120)
        let r = e.update(pct: 92, reset: 999_999_999, now: 180)
        XCTAssertNotNil(r.capEpoch)
        XCTAssertEqual(r.style, .warn)
    }
}
