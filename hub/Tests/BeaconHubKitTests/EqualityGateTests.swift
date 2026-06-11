import XCTest
@testable import BeaconHubKit

// #59 gates the statusline fan-out on `!=` against the last published value. That gate is only correct
// if Equatable distinguishes every field the device/UI consumes -- a future custom `==` that drops a
// field would make the gate silently swallow real updates. These tables pin the contract: each
// single-field mutation MUST compare unequal, and an untouched copy MUST compare equal.
final class EqualityGateTests: XCTestCase {

    func testBuddyStateGateContract() {
        let base = BuddyState(running: 1, waiting: 1, tokens: 100, contextPct: 40,
                              entries: ["10:42 git push"],
                              prompt: BuddyPrompt(id: "req_a", tool: "Bash", hint: "ls"))
        let mutations: [(name: String, value: BuddyState)] = [
            ("running",  { var b = base; b.running = 2; return b }()),
            ("waiting",  { var b = base; b.waiting = 0; return b }()),
            ("tokens",   { var b = base; b.tokens = 101; return b }()),
            ("ctxPct",   { var b = base; b.contextPct = 41; return b }()),
            ("entries",  { var b = base; b.entries = ["10:43 git push"]; return b }()),
            ("promptId", { var b = base; b.prompt?.id = "req_b"; return b }()),
            ("promptNil",{ var b = base; b.prompt = nil; return b }()),
        ]
        XCTAssertEqual(base, base)   // identical copy -> gate skips (no redundant resend).
        for m in mutations {
            XCTAssertNotEqual(base, m.value, "BuddyState.\(m.name) change must defeat the equality gate")
        }
    }

    func testProviderUsageGateContract() {
        let base = ProviderUsage(h5: UsageWindow(pct: 24, reset: 1_717_600_000),
                                 d7: UsageWindow(pct: 32, reset: 1_717_800_000))
        let mutations: [(name: String, value: ProviderUsage)] = [
            ("h5.pct",   { var p = base; p.h5.pct = 25; return p }()),
            ("h5.reset", { var p = base; p.h5.reset = 1_717_600_001; return p }()),
            ("h5.nilPct",{ var p = base; p.h5.pct = nil; return p }()),
            ("d7.pct",   { var p = base; p.d7.pct = 33; return p }()),
            ("d7.reset", { var p = base; p.d7.reset = 1_717_800_001; return p }()),
        ]
        XCTAssertEqual(base, base)
        for m in mutations {
            XCTAssertNotEqual(base, m.value, "ProviderUsage.\(m.name) change must defeat the equality gate")
        }
        // The poller's "no data" sentinel and a real zero-pct window must not collapse to equal.
        XCTAssertNotEqual(ProviderUsage.unavailable,
                          ProviderUsage(h5: UsageWindow(pct: 0, reset: 0), d7: UsageWindow(pct: 0, reset: 0)))
    }
}
