import Foundation

// Per-message Claude token breakdown (from message.usage). Tokens are additive across messages.
public struct ClaudeUsage: Equatable {
    public var inputTokens: Int        // uncached input
    public var cacheReadTokens: Int    // cache_read_input_tokens
    public var cacheWrite5mTokens: Int // cache_creation.ephemeral_5m_input_tokens
    public var cacheWrite1hTokens: Int // cache_creation.ephemeral_1h_input_tokens
    public var outputTokens: Int
    public init(inputTokens: Int = 0, cacheReadTokens: Int = 0, cacheWrite5mTokens: Int = 0,
                cacheWrite1hTokens: Int = 0, outputTokens: Int = 0) {
        self.inputTokens = inputTokens; self.cacheReadTokens = cacheReadTokens
        self.cacheWrite5mTokens = cacheWrite5mTokens; self.cacheWrite1hTokens = cacheWrite1hTokens
        self.outputTokens = outputTokens
    }
    // Total raw tokens for the "X tok" UI label (sum of every billed bucket).
    public var totalTokens: Int { inputTokens + cacheReadTokens + cacheWrite5mTokens + cacheWrite1hTokens + outputTokens }
    public static func + (a: ClaudeUsage, b: ClaudeUsage) -> ClaudeUsage {
        ClaudeUsage(inputTokens: a.inputTokens + b.inputTokens,
                    cacheReadTokens: a.cacheReadTokens + b.cacheReadTokens,
                    cacheWrite5mTokens: a.cacheWrite5mTokens + b.cacheWrite5mTokens,
                    cacheWrite1hTokens: a.cacheWrite1hTokens + b.cacheWrite1hTokens,
                    outputTokens: a.outputTokens + b.outputTokens)
    }
}

// Per-turn Codex token delta (from payload.info.last_token_usage). cached ⊆ input, reasoning ⊆ output.
public struct CodexUsage: Equatable {
    public var inputTokens: Int
    public var cachedInputTokens: Int
    public var outputTokens: Int          // already includes reasoning_output_tokens
    public var reasoningOutputTokens: Int // kept for display only; NOT billed separately
    public init(inputTokens: Int = 0, cachedInputTokens: Int = 0, outputTokens: Int = 0, reasoningOutputTokens: Int = 0) {
        self.inputTokens = inputTokens; self.cachedInputTokens = cachedInputTokens
        self.outputTokens = outputTokens; self.reasoningOutputTokens = reasoningOutputTokens
    }
    public var totalTokens: Int { inputTokens + outputTokens }   // verified invariant
    public static func + (a: CodexUsage, b: CodexUsage) -> CodexUsage {
        CodexUsage(inputTokens: a.inputTokens + b.inputTokens,
                   cachedInputTokens: a.cachedInputTokens + b.cachedInputTokens,
                   outputTokens: a.outputTokens + b.outputTokens,
                   reasoningOutputTokens: a.reasoningOutputTokens + b.reasoningOutputTokens)
    }
}

// A per-model aggregate for one period: raw token count + estimated cost (nil if model unpriced).
public struct ModelCostRow: Equatable {
    public let model: String
    public let tokens: Int
    public let costUSD: Double?   // nil => unpriced model, shown as "-" and excluded from total
    public init(model: String, tokens: Int, costUSD: Double?) {
        self.model = model; self.tokens = tokens; self.costUSD = costUSD
    }
}

// The cost card payload for a selected period: per-model rows + total of the priced rows.
public struct CostBreakdown: Equatable {
    public let rows: [ModelCostRow]       // sorted by cost desc, unpriced last
    public let totalUSD: Double           // sum of priced rows only
    public init(rows: [ModelCostRow], totalUSD: Double) { self.rows = rows; self.totalUSD = totalUSD }
}
