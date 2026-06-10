import Foundation

public enum CostMath {
    public static func cost(claude u: ClaudeUsage, pricing p: ModelPricing) -> Double {
        (Double(u.inputTokens) * p.inputPerM
            + Double(u.cacheReadTokens) * p.cacheReadPerM
            + Double(u.cacheWrite5mTokens) * p.cacheWrite5mPerM
            + Double(u.cacheWrite1hTokens) * p.cacheWrite1hPerM
            + Double(u.outputTokens) * p.outputPerM) / 1_000_000
    }

    public static func cost(codex u: CodexUsage, pricing p: ModelPricing) -> Double {
        let uncachedInput = max(0, u.inputTokens - u.cachedInputTokens)
        return (Double(uncachedInput) * p.inputPerM
            + Double(u.cachedInputTokens) * p.cacheReadPerM
            + Double(u.outputTokens) * p.outputPerM) / 1_000_000   // reasoning already in outputTokens
    }
}
