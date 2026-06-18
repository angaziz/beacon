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

    // The display-side freshness gate (#93) must mirror the poll gate so the displayed Claude value and
    // the Keychain-skip decision never disagree: statuslineFresh is exactly the inverse of shouldPollClaude.
    func testStatuslineFresh() {
        let cases: [(name: String, age: TimeInterval?, interval: TimeInterval, want: Bool)] = [
            ("never received => stale",      nil, 45, false),
            ("just received => fresh",         1, 45, true),
            ("within 2x interval => fresh",   89, 45, true),
            ("at 2x interval => stale",       90, 45, false),
            ("long stale => stale",          600, 45, false),
        ]
        for c in cases {
            XCTAssertEqual(
                UsagePollDecision.statuslineFresh(age: c.age, interval: c.interval),
                c.want, c.name)
            // Complementary by construction: fresh display <=> skip the Claude poll.
            XCTAssertNotEqual(
                UsagePollDecision.statuslineFresh(age: c.age, interval: c.interval),
                UsagePollDecision.shouldPollClaude(statuslineAge: c.age, interval: c.interval), c.name)
        }
    }

    func testShouldRereadCredential() {
        let cases: [(name: String, since: TimeInterval?, cooldown: TimeInterval, want: Bool)] = [
            ("never read => read",          nil, 300, true),
            ("just read => skip",             1, 300, false),
            ("within cooldown => skip",     299, 300, false),
            ("at cooldown => read",         300, 300, true),
            ("past cooldown => read",       301, 300, true),
        ]
        for c in cases {
            XCTAssertEqual(
                UsagePollDecision.shouldRereadCredential(secondsSinceLastRead: c.since, cooldown: c.cooldown),
                c.want, c.name)
        }
    }
}
