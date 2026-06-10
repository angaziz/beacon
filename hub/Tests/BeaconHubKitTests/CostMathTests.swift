import XCTest
@testable import BeaconHubKit

final class CostMathTests: XCTestCase {
    func testClaudeCostWithCacheTiers() {
        let usage = ClaudeUsage(inputTokens: 1_000_000, cacheReadTokens: 1_000_000,
                                cacheWrite5mTokens: 1_000_000, cacheWrite1hTokens: 1_000_000,
                                outputTokens: 1_000_000)
        let p = PricingTable.shared.pricing(for: "claude-opus-4-8")!
        // 5 + 0.5 + 6.25 + 10 + 25 = 46.75
        XCTAssertEqual(CostMath.cost(claude: usage, pricing: p), 46.75, accuracy: 1e-9)
    }

    func testClaudeFractionalTokens() {
        let usage = ClaudeUsage(inputTokens: 12_902, cacheReadTokens: 16_371,
                                cacheWrite5mTokens: 0, cacheWrite1hTokens: 3_106, outputTokens: 229)
        let p = PricingTable.shared.pricing(for: "claude-opus-4-8")!
        // 12902*5 + 16371*0.5 + 3106*10 + 229*25 = 109480.5, /1e6
        let want: Double = 109_480.5 / 1_000_000
        XCTAssertEqual(CostMath.cost(claude: usage, pricing: p), want, accuracy: 1e-9)
    }

    func testCodexCostUsesDeltaAndNoDoubleCountReasoning() {
        let usage = CodexUsage(inputTokens: 15_510, cachedInputTokens: 3_456,
                               outputTokens: 426, reasoningOutputTokens: 277)
        let p = PricingTable.shared.pricing(for: "gpt-5.5")!
        // uncached 12054 @5 + cached 3456 @0.5 + output 426 @30 (reasoning 277 INSIDE 426) = 74778, /1e6
        let want: Double = 74_778.0 / 1_000_000
        XCTAssertEqual(CostMath.cost(codex: usage, pricing: p), want, accuracy: 1e-9)
    }
}
