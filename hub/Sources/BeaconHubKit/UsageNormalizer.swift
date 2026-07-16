import Foundation

// Normalize the two providers' unofficial usage responses into the §7.2 schema. Pure functions over
// raw bytes so they are unit-tested against recorded fixtures (hub/CONTRACT.md §C, real captures
// confirmed 2026-06-11). Endpoints/shapes are unofficial -- isolate them here and expect breakage; we
// read only the fields below and ignore the rest, so extra/added keys are harmless.
public enum UsageNormalizer {

    // Claude: GET api.anthropic.com/api/oauth/usage
    //   { five_hour:{utilization,resets_at(ISO)}, seven_day:{utilization,resets_at}, ... }
    // utilization is already percent-of-limit (a Double, e.g. 24.0).
    public static func claude(_ raw: Data) -> ProviderUsage? {
        guard let obj = try? JSONSerialization.jsonObject(with: raw) as? [String: Any] else { return nil }
        guard let h5 = window(obj["five_hour"], pctKey: "utilization", resetKey: "resets_at"),
              let d7 = window(obj["seven_day"], pctKey: "utilization", resetKey: "resets_at")
        else { return nil }
        return ProviderUsage(h5: h5, d7: d7)
    }

    // Codex: GET chatgpt.com/backend-api/wham/usage
    //   { rate_limit:{ primary_window:{used_percent,limit_window_seconds,reset_at(epoch)},
    //                  secondary_window:{...}|null } }
    // Plans without a 5h limit (observed on Plus, 2026-07-16) send secondary_window:null and a
    // primary_window that is the 7d window -- so windows are routed by limit_window_seconds, not
    // position. Positional fallback (primary=5h, secondary=7d) covers responses lacking that key.
    // A missing window normalizes to pct:nil (device renders "--"); no windows at all => nil.
    public static func codex(_ raw: Data) -> ProviderUsage? {
        guard let obj = try? JSONSerialization.jsonObject(with: raw) as? [String: Any],
              let rl = obj["rate_limit"] as? [String: Any] else { return nil }
        var h5: UsageWindow?
        var d7: UsageWindow?
        for (key, fallbackIsH5) in [("primary_window", true), ("secondary_window", false)] {
            guard let dict = rl[key] as? [String: Any],
                  let w = window(dict, pctKey: "used_percent", resetKey: "reset_at") else { continue }
            let isH5 = number(dict["limit_window_seconds"]).map { $0 <= 6 * 3600 } ?? fallbackIsH5
            if isH5 { h5 = h5 ?? w } else { d7 = d7 ?? w }
        }
        guard h5 != nil || d7 != nil else { return nil }
        let absent = UsageWindow(pct: nil, reset: 0)
        return ProviderUsage(h5: h5 ?? absent, d7: d7 ?? absent)
    }

    // --- helpers ---

    private static func window(_ any: Any?, pctKey: String, resetKey: String) -> UsageWindow? {
        guard let w = any as? [String: Any] else { return nil }
        let pct = number(w[pctKey]).map { Int(($0).rounded()) }
        let reset = epoch(w[resetKey])
        return UsageWindow(pct: pct.map { max(0, min(100, $0)) }, reset: reset)
    }

    private static func number(_ any: Any?) -> Double? {
        if let d = any as? Double { return d }
        if let i = any as? Int { return Double(i) }
        if let s = any as? String, let d = Double(s) { return d }
        return nil
    }

    // Hoisted formatters (#66 L7): one instance each instead of constructing a formatter per parse.
    // formatOptions are set once and never mutated, so concurrent date(from:) from the poller's parallel
    // Claude/Codex fetch completions is safe (ISO8601DateFormatter reads are thread-safe).
    private static let isoFractional: ISO8601DateFormatter = {
        let f = ISO8601DateFormatter(); f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]; return f
    }()
    private static let isoPlain: ISO8601DateFormatter = {
        let f = ISO8601DateFormatter(); f.formatOptions = [.withInternetDateTime]; return f
    }()

    // Accept epoch seconds (Int/Double) or an ISO-8601 string; 0 if absent/unparseable.
    private static func epoch(_ any: Any?) -> Int {
        if let i = any as? Int { return i }
        if let d = any as? Double { return Int(d) }
        if let s = any as? String {
            if let i = Int(s) { return i }
            if let date = isoFractional.date(from: s) { return Int(date.timeIntervalSince1970) }
            if let date = isoPlain.date(from: s) { return Int(date.timeIntervalSince1970) }
        }
        return 0
    }
}
