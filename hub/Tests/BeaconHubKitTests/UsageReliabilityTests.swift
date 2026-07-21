import XCTest
@testable import BeaconHubKit

// #108: backoff/retention reliability. Pure, table-driven over (priorState, outcome, now) so the whole
// 429 last-good path is host-tested without URLSession/timers.
final class UsageReliabilityTests: XCTestCase {

    // --- exponential backoff ---

    func testBackoff() {
        let base: TimeInterval = 45, cap: TimeInterval = 900
        let cases: [(fails: Int, want: TimeInterval)] = [
            (0, 0), (1, 45), (2, 90), (3, 180), (4, 360), (5, 720),
            (6, 900),    // 1440 clamped to cap
            (10, 900),   // far past cap
        ]
        for c in cases {
            XCTAssertEqual(UsagePollDecision.backoff(consecutiveFails: c.fails, base: base, cap: cap),
                           c.want, "fails=\(c.fails)")
        }
    }

    // --- delay composition: Retry-After floor + jitter, two caps ---

    func testPollDelay() {
        let base: TimeInterval = 45, cap: TimeInterval = 900, sanity: TimeInterval = 3600
        func d(_ fails: Int, _ ra: TimeInterval?, jitter: Double = 0) -> TimeInterval {
            UsagePollDecision.pollDelay(consecutiveFails: fails, retryAfter: ra, base: base, cap: cap,
                                        retryAfterSanityCap: sanity, jitterFraction: jitter)
        }
        XCTAssertEqual(d(1, nil), 45)                 // no Retry-After => backoff
        XCTAssertEqual(d(1, 120), 120)                // Retry-After floor wins over 45
        XCTAssertEqual(d(3, 60), 180)                 // backoff (180) wins over small Retry-After
        XCTAssertEqual(d(1, 3600), 3600)              // long server cooldown honored, NOT clamped to 900 cap
        XCTAssertEqual(d(1, 99999), 3600)             // absurd Retry-After clamped to the 60-min sanity cap
        XCTAssertEqual(d(1, -5), 45)                  // negative Retry-After rejected => backoff
        // Negative jitter can never schedule before the server-mandated floor.
        XCTAssertEqual(d(1, 100, jitter: -0.2), 100)
        // Positive jitter widens the backoff when there is no floor.
        XCTAssertEqual(d(1, nil, jitter: 0.2), 54, accuracy: 0.001)
    }

    // --- per-window expiry ---

    func testWindowExpired() {
        let now = Date(timeIntervalSince1970: 1_000_000)
        let maxStale: TimeInterval = 1800
        let nowEpoch = Int(now.timeIntervalSince1970)
        let cases: [(name: String, at: Date?, reset: Int, want: Bool)] = [
            ("nil lastGoodAt",        nil,             nowEpoch + 1000, true),
            ("fresh, reset future",   now.addingTimeInterval(-100), nowEpoch + 1000, false),
            ("past maxStale",         now.addingTimeInterval(-2000), nowEpoch + 1000, true),
            ("reset passed",          now.addingTimeInterval(-100), nowEpoch - 10,   true),
            ("reset unknown (0)",     now.addingTimeInterval(-100), 0,               false),
            ("at maxStale boundary",  now.addingTimeInterval(-1800), nowEpoch + 1000, false),
        ]
        for c in cases {
            XCTAssertEqual(
                UsagePollDecision.windowExpired(lastGoodAt: c.at, now: now, maxStale: maxStale, windowReset: c.reset),
                c.want, c.name)
        }
    }

    // --- reducer ---

    private let now = Date(timeIntervalSince1970: 1_000_000)
    private var futureReset: Int { Int(now.timeIntervalSince1970) + 100_000 }
    private func good() -> ProviderUsage {
        ProviderUsage(h5: UsageWindow(pct: 24, reset: futureReset),
                      d7: UsageWindow(pct: 32, reset: futureReset))
    }

    func testLiveStoresLastGoodAndClearsStale() {
        let incoming = ProviderUsage(h5: good().h5, d7: good().d7, stale: true)   // a stray stale must be cleared.
        let r = UsageReducer.reduceProvider(prior: ProviderRetention(), outcome: .live,
                                            usage: incoming, now: now, maxStale: 1800, label: "Claude")
        XCTAssertNil(r.display.usage.stale)
        XCTAssertNil(r.display.note)
        XCTAssertEqual(r.next.lastGood?.stale, .none)
        XCTAssertEqual(r.next.lastGoodAt, now)
        XCTAssertEqual(r.next.lastGood?.h5.pct, 24)
    }

    func testTerminalClearsLastGood() {
        let prior = ProviderRetention(lastGood: good(), lastGoodAt: now.addingTimeInterval(-10))
        let r = UsageReducer.reduceProvider(prior: prior, outcome: .terminal(reason: "re-login", kind: .other),
                                            usage: .unavailable, now: now, maxStale: 1800, label: "Claude")
        XCTAssertNil(r.next.lastGood)                 // credential-identity safety.
        XCTAssertNil(r.next.lastGoodAt)
        XCTAssertEqual(r.display.usage, .unavailable)
        XCTAssertEqual(r.display.note, UsageNote(severity: .error, text: "re-login"))
    }

    func testTransientWithoutPriorBlanks() {
        let r = UsageReducer.reduceProvider(prior: ProviderRetention(),
                                            outcome: .transient(retryAfter: nil, reason: "HTTP 429"),
                                            usage: .unavailable, now: now, maxStale: 1800, label: "Claude")
        XCTAssertEqual(r.display.usage, .unavailable)
        XCTAssertEqual(r.display.note?.severity, .error)
        XCTAssertNil(r.next.lastGood)
    }

    func testTransientFreshServesStale() {
        let prior = ProviderRetention(lastGood: good(), lastGoodAt: now.addingTimeInterval(-100))
        let r = UsageReducer.reduceProvider(prior: prior,
                                            outcome: .transient(retryAfter: nil, reason: "HTTP 429"),
                                            usage: .unavailable, now: now, maxStale: 1800, label: "Claude")
        XCTAssertEqual(r.display.usage.stale, true)
        XCTAssertEqual(r.display.usage.h5.pct, 24)
        XCTAssertEqual(r.display.usage.d7.pct, 32)
        XCTAssertEqual(r.display.note?.severity, .info)
        XCTAssertEqual(r.next.lastGood?.h5.pct, 24)   // retention unchanged on transient.
    }

    func testTransientPerWindowExpiry() {
        // h5 window has reset; d7 still valid -> h5 blanks, d7 retained, provider still stale.
        let mixed = ProviderUsage(h5: UsageWindow(pct: 24, reset: Int(now.timeIntervalSince1970) - 10),
                                  d7: UsageWindow(pct: 32, reset: futureReset))
        let prior = ProviderRetention(lastGood: mixed, lastGoodAt: now.addingTimeInterval(-100))
        let r = UsageReducer.reduceProvider(prior: prior,
                                            outcome: .transient(retryAfter: nil, reason: "HTTP 429"),
                                            usage: .unavailable, now: now, maxStale: 1800, label: "Claude")
        XCTAssertNil(r.display.usage.h5.pct)          // h5 reset passed => "--"
        XCTAssertEqual(r.display.usage.d7.pct, 32)    // d7 retained
        XCTAssertEqual(r.display.usage.stale, true)
        XCTAssertEqual(r.display.note?.severity, .info)
    }

    func testTransientFullyExpiredBlanks() {
        let prior = ProviderRetention(lastGood: good(), lastGoodAt: now.addingTimeInterval(-2000))  // > maxStale
        let r = UsageReducer.reduceProvider(prior: prior,
                                            outcome: .transient(retryAfter: nil, reason: "HTTP 429"),
                                            usage: .unavailable, now: now, maxStale: 1800, label: "Claude")
        XCTAssertNil(r.display.usage.h5.pct)
        XCTAssertNil(r.display.usage.d7.pct)
        XCTAssertNil(r.display.usage.stale)           // nothing served => not stale, just unavailable.
        XCTAssertEqual(r.display.note?.severity, .error)
        XCTAssertNotNil(r.next.lastGood)              // transient never clears (only terminal does).
    }

    // --- Retry-After header parsing ---

    func testRetryAfterParse() {
        let now = Date(timeIntervalSince1970: 1_000_000)
        XCTAssertNil(RetryAfter.parse(nil, now: now))
        XCTAssertNil(RetryAfter.parse("", now: now))
        XCTAssertNil(RetryAfter.parse("abc", now: now))
        XCTAssertEqual(RetryAfter.parse("120", now: now), 120)
        XCTAssertEqual(RetryAfter.parse("  90 ", now: now), 90)
        XCTAssertEqual(RetryAfter.parse("0", now: now), 0)
        XCTAssertEqual(RetryAfter.parse("-5", now: now), 0)   // negative delay clamps to 0.

        let fmt = DateFormatter()
        fmt.locale = Locale(identifier: "en_US_POSIX")
        fmt.timeZone = TimeZone(identifier: "GMT")
        fmt.dateFormat = "EEE, dd MMM yyyy HH:mm:ss zzz"
        let future = fmt.string(from: now.addingTimeInterval(300))
        XCTAssertEqual(RetryAfter.parse(future, now: now) ?? -1, 300, accuracy: 1)
        let past = fmt.string(from: now.addingTimeInterval(-300))
        XCTAssertEqual(RetryAfter.parse(past, now: now), 0)   // past HTTP-date clamps to 0.
    }

    func testInactiveClearsLastGoodWithMutedNote() {
        let prior = ProviderRetention(lastGood: good(), lastGoodAt: now.addingTimeInterval(-10))
        let r = UsageReducer.reduceProvider(prior: prior, outcome: .inactive(reason: "Claude inactive"),
                                            usage: .unavailable, now: now, maxStale: 1800, label: "Claude")
        XCTAssertNil(r.next.lastGood)                 // identity safety, same as terminal.
        XCTAssertNil(r.next.lastGoodAt)
        XCTAssertEqual(r.display.usage, .unavailable)
        XCTAssertEqual(r.display.note, UsageNote(severity: .info, text: "Claude inactive"))   // muted, not red.
    }

    func testVisibleNotesFiltersDisabledAndPreservesOrder() {
        let a = UsageNote(severity: .error, text: "a")
        let b = UsageNote(severity: .info, text: "b")
        let notes = ["claude": a, "codex": b]
        // A usage-disabled provider's (possibly frozen) note is filtered out (#126).
        XCTAssertEqual(UsageReducer.visibleNotes(order: ["claude", "codex"], notes: notes,
                                                 enabled: { $0 != "codex" }), [a])
        // All enabled => registration order preserved.
        XCTAssertEqual(UsageReducer.visibleNotes(order: ["claude", "codex"], notes: notes,
                                                 enabled: { _ in true }), [a, b])
        // An enabled id with no note is skipped, no crash.
        XCTAssertEqual(UsageReducer.visibleNotes(order: ["claude", "missing"], notes: notes,
                                                 enabled: { _ in true }), [a])
    }
}
