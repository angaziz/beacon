import XCTest
@testable import BeaconHubKit

final class CostAggregatorTests: XCTestCase {
    private func cal(_ tz: String) -> Calendar {
        var c = Calendar(identifier: .gregorian)
        c.timeZone = TimeZone(identifier: tz)!
        c.firstWeekday = 2   // Monday
        return c
    }

    func testDedupByKey() {
        var agg = CostAggregator(calendar: cal("UTC"))
        let u = ClaudeUsage(inputTokens: 100, outputTokens: 10)
        agg.addClaude(ClaudeTurn(dedupKey: "req_1", model: "claude-opus-4-8", epochSeconds: 1_780_000_000, usage: u))
        agg.addClaude(ClaudeTurn(dedupKey: "req_1", model: "claude-opus-4-8", epochSeconds: 1_780_000_000, usage: u)) // dup
        let bd = agg.breakdown(period: .day, now: 1_780_000_000, table: PricingTable.shared)
        XCTAssertEqual(bd.rows.count, 1)
        XCTAssertEqual(bd.rows.first?.tokens, 110)
    }

    func testPeriodBucketingAcrossMidnightLocalTZ() {
        var a = CostAggregator(calendar: cal("Asia/Jakarta"))
        // 2026-06-10 23:30 +07 (local day Jun 10) == epoch 1781109000 (verified)
        a.addCodex(model: "gpt-5.5", turn: CodexTurn(epochSeconds: 1_781_109_000, usage: CodexUsage(inputTokens: 1000, outputTokens: 10)))
        // 2026-06-11 00:30 +07 (local day Jun 11) == epoch 1781112600 (verified)
        a.addCodex(model: "gpt-5.5", turn: CodexTurn(epochSeconds: 1_781_112_600, usage: CodexUsage(inputTokens: 2000, outputTokens: 20)))
        let now = 1_781_114_400   // 2026-06-11 01:00 +07 (verified)
        let today = a.breakdown(period: .day, now: now, table: PricingTable.shared)
        XCTAssertEqual(today.rows.first?.tokens, 2020)   // only the Jun-11 turn
    }

    func testUnpricedModelExcludedFromTotalShownAsNilCost() {
        var agg = CostAggregator(calendar: cal("UTC"))
        agg.addClaude(ClaudeTurn(dedupKey: "k1", model: "claude-opus-4-8", epochSeconds: 1_780_000_000,
                                 usage: ClaudeUsage(inputTokens: 1_000_000)))
        agg.addClaude(ClaudeTurn(dedupKey: "k2", model: "mystery-model", epochSeconds: 1_780_000_000,
                                 usage: ClaudeUsage(inputTokens: 1_000_000)))
        let bd = agg.breakdown(period: .day, now: 1_780_000_000, table: PricingTable.shared)
        XCTAssertEqual(bd.totalUSD, 5.0, accuracy: 1e-9)   // only the opus row
        XCTAssertNil(bd.rows.first(where: { $0.model == "mystery-model" })?.costUSD)
    }

    func testRollingSixHourWindow() {
        var agg = CostAggregator(calendar: cal("UTC"))
        let now = 1_780_000_000
        agg.addClaude(ClaudeTurn(dedupKey: "old", model: "claude-opus-4-8", epochSeconds: now - 7*3600, usage: ClaudeUsage(inputTokens: 1000)))
        agg.addClaude(ClaudeTurn(dedupKey: "new", model: "claude-opus-4-8", epochSeconds: now - 1*3600, usage: ClaudeUsage(inputTokens: 2000)))
        let bd = agg.breakdown(period: .last6h, now: now, table: PricingTable.shared)
        XCTAssertEqual(bd.rows.first?.tokens, 2000)
    }
}
