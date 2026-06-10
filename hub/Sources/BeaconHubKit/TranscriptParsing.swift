import Foundation

// A parsed, attributable transcript turn. epochSeconds is the floored Unix time of the turn (local-day
// bucketing happens later in CostAggregator). dedupKey survives resumed-session duplication.
public struct ClaudeTurn: Equatable {
    public let dedupKey: String
    public let model: String
    public let epochSeconds: Int
    public let usage: ClaudeUsage
}

public struct CodexTurn: Equatable {
    public let epochSeconds: Int
    public let usage: CodexUsage   // model is per-file (turn_context); attached by the reader
}

// Backfill sample seeded from a Codex token_count's rate_limits (h5=primary 300m, d7=secondary 10080m).
public struct CodexRateSample: Equatable {
    public let epochSeconds: Int
    public let h5Pct: Int?; public let h5Reset: Int
    public let d7Pct: Int?; public let d7Reset: Int
}

private func epoch(fromISO s: String?) -> Int? {
    guard let s else { return nil }
    let f = ISO8601DateFormatter()
    f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
    if let d = f.date(from: s) { return Int(d.timeIntervalSince1970) }
    f.formatOptions = [.withInternetDateTime]
    if let d = f.date(from: s) { return Int(d.timeIntervalSince1970) }
    return nil
}

private func intVal(_ any: Any?) -> Int? {
    if let i = any as? Int { return i }
    if let d = any as? Double { return Int(d) }
    if let s = any as? String, let i = Int(s) { return i }
    return nil
}

public enum ClaudeTranscriptParser {
    public static func parseLine(_ line: [String: Any]) -> ClaudeTurn? {
        guard (line["type"] as? String) == "assistant",
              let message = line["message"] as? [String: Any],
              let model = message["model"] as? String, model != "<synthetic>",
              let usageObj = message["usage"] as? [String: Any] else { return nil }
        guard let key = (line["requestId"] as? String) ?? (line["uuid"] as? String) else { return nil }
        let ts = epoch(fromISO: line["timestamp"] as? String) ?? 0
        let cc = usageObj["cache_creation"] as? [String: Any]
        let usage = ClaudeUsage(
            inputTokens: intVal(usageObj["input_tokens"]) ?? 0,
            cacheReadTokens: intVal(usageObj["cache_read_input_tokens"]) ?? 0,
            cacheWrite5mTokens: intVal(cc?["ephemeral_5m_input_tokens"]) ?? 0,
            cacheWrite1hTokens: intVal(cc?["ephemeral_1h_input_tokens"]) ?? 0,
            outputTokens: intVal(usageObj["output_tokens"]) ?? 0)
        return ClaudeTurn(dedupKey: key, model: model, epochSeconds: ts, usage: usage)
    }
}

public enum CodexTranscriptParser {
    // The model lives in a once-per-file turn_context record.
    public static func parseModel(_ line: [String: Any]) -> String? {
        guard (line["type"] as? String) == "turn_context",
              let payload = line["payload"] as? [String: Any] else { return nil }
        return payload["model"] as? String
    }

    public static func parseLine(_ line: [String: Any]) -> CodexTurn? {
        guard (line["type"] as? String) == "event_msg",
              let payload = line["payload"] as? [String: Any],
              (payload["type"] as? String) == "token_count",
              let info = payload["info"] as? [String: Any],       // null on a session's first event
              let last = info["last_token_usage"] as? [String: Any] else { return nil }
        let ts = epoch(fromISO: line["timestamp"] as? String) ?? 0
        let usage = CodexUsage(
            inputTokens: intVal(last["input_tokens"]) ?? 0,
            cachedInputTokens: intVal(last["cached_input_tokens"]) ?? 0,
            outputTokens: intVal(last["output_tokens"]) ?? 0,
            reasoningOutputTokens: intVal(last["reasoning_output_tokens"]) ?? 0)
        return CodexTurn(epochSeconds: ts, usage: usage)
    }

    public static func parseRateLimits(_ line: [String: Any]) -> CodexRateSample? {
        guard (line["type"] as? String) == "event_msg",
              let payload = line["payload"] as? [String: Any],
              (payload["type"] as? String) == "token_count",
              let rl = payload["rate_limits"] as? [String: Any] else { return nil }
        let primary = rl["primary"] as? [String: Any]
        let secondary = rl["secondary"] as? [String: Any]
        guard primary != nil || secondary != nil else { return nil }   // null on empty events
        func pct(_ d: [String: Any]?) -> Int? {
            guard let v = intVal(d?["used_percent"]).map(Double.init) ?? (d?["used_percent"] as? Double)
            else { return nil }
            return max(0, min(100, Int(v.rounded())))
        }
        let ts = epoch(fromISO: line["timestamp"] as? String) ?? 0
        return CodexRateSample(epochSeconds: ts,
                               h5Pct: pct(primary), h5Reset: intVal(primary?["resets_at"]) ?? 0,
                               d7Pct: pct(secondary), d7Reset: intVal(secondary?["resets_at"]) ?? 0)
    }
}
