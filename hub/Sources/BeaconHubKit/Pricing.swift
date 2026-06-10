import Foundation

// Hardcoded per-model list prices, per 1,000,000 tokens (issue #57). Cache tiers derive from the input
// rate by the published multipliers: read 0.1x, ephemeral-5m write 1.25x, ephemeral-1h write 2x. Prices
// drift; this table is maintained by hand. Verified 2026-06-10 against platform.claude.com and
// openai.com/api/pricing. Unknown models return nil => counted but cost shown as "-" and excluded from totals.
public struct ModelPricing: Equatable {
    public let inputPerM: Double
    public let cacheReadPerM: Double
    public let cacheWrite5mPerM: Double
    public let cacheWrite1hPerM: Double
    public let outputPerM: Double

    // Claude convention: cache tiers are fixed multiples of the input rate.
    static func claude(input: Double, output: Double) -> ModelPricing {
        ModelPricing(inputPerM: input, cacheReadPerM: input * 0.1,
                     cacheWrite5mPerM: input * 1.25, cacheWrite1hPerM: input * 2.0,
                     outputPerM: output)
    }
    // Codex/OpenAI: published cached-input rate, no cache-write tiers (unused for gpt-5.5).
    static func openai(input: Double, cachedInput: Double, output: Double) -> ModelPricing {
        ModelPricing(inputPerM: input, cacheReadPerM: cachedInput,
                     cacheWrite5mPerM: 0, cacheWrite1hPerM: 0, outputPerM: output)
    }
}

public struct PricingTable {
    public static let shared = PricingTable()

    // Exact model strings verified in local transcripts, plus bare aliases Claude Code can emit
    // ("sonnet" seen on disk; "opus"/"haiku" added defensively). Alias maps to the current dated model.
    private let table: [String: ModelPricing] = [
        "claude-opus-4-8":            .claude(input: 5.0,  output: 25.0),
        "claude-sonnet-4-6":          .claude(input: 3.0,  output: 15.0),
        "claude-haiku-4-5-20251001":  .claude(input: 1.0,  output: 5.0),
        "claude-fable-5":             .claude(input: 10.0, output: 50.0),
        "gpt-5.5":                    .openai(input: 5.0, cachedInput: 0.5, output: 30.0),
    ]
    private let aliases: [String: String] = [
        "opus":   "claude-opus-4-8",
        "sonnet": "claude-sonnet-4-6",
        "haiku":  "claude-haiku-4-5-20251001",
        "fable":  "claude-fable-5",
    ]

    public func pricing(for model: String) -> ModelPricing? {
        if let p = table[model] { return p }
        if let canonical = aliases[model] { return table[canonical] }
        return nil
    }
}
