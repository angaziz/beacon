import XCTest
@testable import BeaconHubKit

final class PairingEscalationTests: XCTestCase {

    // Trips on the maxAttempts-th first-time failure (time held constant so the deadline never fires).
    func testTripsAtMaxAttempts() {
        var e = PairingEscalation(maxAttempts: 4, deadline: 25)
        e.recordAttemptStart(now: 0)
        let results = (1...4).map { _ in e.recordFailure(now: 0, hadConnection: false) }
        XCTAssertEqual(results, [false, false, false, true])
    }

    // Trips on a single slow attempt once elapsed >= deadline (attempt budget not yet burned).
    func testTripsAtDeadline() {
        var e = PairingEscalation(maxAttempts: 4, deadline: 25)
        e.recordAttemptStart(now: 100)
        XCTAssertFalse(e.recordFailure(now: 110, hadConnection: false))   // 10s elapsed, under deadline
        XCTAssertTrue(e.recordFailure(now: 125, hadConnection: false))    // 25s elapsed, at deadline
    }

    // A previously-good device (hadConnection true) never escalates, no matter how many failures.
    func testNeverEscalatesWhenHadConnection() {
        var e = PairingEscalation(maxAttempts: 2, deadline: 1)
        e.recordAttemptStart(now: 0)
        for _ in 0..<10 {
            XCTAssertFalse(e.recordFailure(now: 1000, hadConnection: true))
        }
    }

    // reset() clears both the counter and the deadline clock (success / try-again / forget).
    func testResetClearsBudget() {
        var e = PairingEscalation(maxAttempts: 2, deadline: 25)
        e.recordAttemptStart(now: 0)
        XCTAssertFalse(e.recordFailure(now: 0, hadConnection: false))
        e.reset()
        e.recordAttemptStart(now: 100)
        XCTAssertFalse(e.recordFailure(now: 100, hadConnection: false))   // counter back to 1, clock restarted
    }

    // Under both thresholds => no escalation.
    func testUnderThresholdDoesNotEscalate() {
        var e = PairingEscalation(maxAttempts: 4, deadline: 25)
        e.recordAttemptStart(now: 0)
        XCTAssertFalse(e.recordFailure(now: 5, hadConnection: false))
        XCTAssertFalse(e.recordFailure(now: 10, hadConnection: false))
    }
}
