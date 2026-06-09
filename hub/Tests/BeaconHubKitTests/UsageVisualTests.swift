import XCTest
@testable import BeaconHubKit

final class UsageVisualTests: XCTestCase {

    func testUsageLevel() {
        let cases: [(pct: Int?, want: UsageLevel)] = [
            (nil, .unavailable),
            (0, .normal), (1, .normal), (69, .normal),
            (70, .elevated), (89, .elevated),
            (90, .critical), (100, .critical), (150, .critical),
        ]
        for c in cases {
            XCTAssertEqual(usageLevel(c.pct), c.want, "pct=\(String(describing: c.pct))")
        }
    }

    func testUsageFillFraction() {
        let cases: [(pct: Int?, want: Double)] = [
            (nil, 0), (-5, 0), (0, 0),
            (50, 0.5), (99, 0.99), (100, 1), (150, 1),
        ]
        for c in cases {
            XCTAssertEqual(usageFillFraction(c.pct), c.want, accuracy: 1e-9,
                           "pct=\(String(describing: c.pct))")
        }
    }

    func testResetDisplay() {
        let now = Date(timeIntervalSince1970: 1_800_000_000)   // fixed reference
        func epoch(_ offset: TimeInterval) -> Int { Int(now.timeIntervalSince1970 + offset) }

        let cases: [(name: String, reset: Int, want: ResetDisplay)] = [
            ("unknown",          0,                      .none),
            ("negative",        -1,                      .none),
            ("elapsed 1s ago",   epoch(-1),              .none),
            ("in 1h",            epoch(3600),            .time(Date(timeIntervalSince1970: TimeInterval(epoch(3600))))),
            ("in 12h",           epoch(12 * 3600),       .time(Date(timeIntervalSince1970: TimeInterval(epoch(12 * 3600))))),
            ("boundary +24h",    epoch(24 * 3600),       .time(Date(timeIntervalSince1970: TimeInterval(epoch(24 * 3600))))),
            ("in 25h",           epoch(25 * 3600),       .weekday(Date(timeIntervalSince1970: TimeInterval(epoch(25 * 3600))))),
        ]
        for c in cases {
            XCTAssertEqual(resetDisplay(reset: c.reset, now: now), c.want, c.name)
        }
    }
}
