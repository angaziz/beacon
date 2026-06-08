import Foundation

// Normalize the two providers' unofficial usage responses into the §7.2 schema. Pure functions over
// raw bytes so they are unit-tested against recorded fixtures (hub/CONTRACT.md). Endpoints/shapes are
// unofficial (docs/research §2.1) -- isolate them here and expect breakage.
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
    //   { rate_limit:{ primary_window:{used_percent,reset_at(epoch)}, secondary_window:{...} } }
    public static func codex(_ raw: Data) -> ProviderUsage? {
        guard let obj = try? JSONSerialization.jsonObject(with: raw) as? [String: Any],
              let rl = obj["rate_limit"] as? [String: Any] else { return nil }
        guard let h5 = window(rl["primary_window"], pctKey: "used_percent", resetKey: "reset_at"),
              let d7 = window(rl["secondary_window"], pctKey: "used_percent", resetKey: "reset_at")
        else { return nil }
        return ProviderUsage(h5: h5, d7: d7)
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

    // Accept epoch seconds (Int/Double) or an ISO-8601 string; 0 if absent/unparseable.
    private static func epoch(_ any: Any?) -> Int {
        if let i = any as? Int { return i }
        if let d = any as? Double { return Int(d) }
        if let s = any as? String {
            if let i = Int(s) { return i }
            let f = ISO8601DateFormatter()
            f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
            if let date = f.date(from: s) { return Int(date.timeIntervalSince1970) }
            f.formatOptions = [.withInternetDateTime]
            if let date = f.date(from: s) { return Int(date.timeIntervalSince1970) }
        }
        return 0
    }
}
