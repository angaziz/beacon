import XCTest
@testable import BeaconHubKit

final class UsagePollDecisionTests: XCTestCase {

    func testShouldPoll() {
        let cases: [(name: String, connected: Bool, since: TimeInterval, backoff: TimeInterval, want: Bool)] = [
            ("connected polls at any age",        true,    1, 300, true),
            ("connected polls immediately",       true,    0, 300, true),
            ("disconnected within backoff skips", false, 100, 300, false),
            ("disconnected at backoff polls",     false, 300, 300, true),
            ("disconnected past backoff polls",   false, 301, 300, true),
        ]
        for c in cases {
            XCTAssertEqual(
                UsagePollDecision.shouldPoll(connected: c.connected, secondsSinceLastPoll: c.since, backoff: c.backoff),
                c.want, c.name)
        }
    }

    func testShouldPollClaude() {
        let cases: [(name: String, age: TimeInterval?, interval: TimeInterval, want: Bool)] = [
            ("never received => poll",  nil, 45, true),
            ("just received => skip",     1, 45, false),
            ("within 2x interval => skip", 89, 45, false),
            ("at 2x interval => poll",    90, 45, true),
            ("long stale => poll",       600, 45, true),
        ]
        for c in cases {
            XCTAssertEqual(
                UsagePollDecision.shouldPollClaude(statuslineAge: c.age, interval: c.interval),
                c.want, c.name)
        }
    }
}
