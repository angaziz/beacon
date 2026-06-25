import Foundation

// Parse an HTTP Retry-After header into seconds-from-now (#108). Two RFC 7231 forms: a delay in
// integer seconds ("120"), or an HTTP-date ("Wed, 21 Oct 2026 07:28:00 GMT") which we convert to a
// delta against `now`. Returns nil when absent/unparseable; a past HTTP-date clamps to 0. Pure over
// (header, now) so it is host-tested; the poller applies its own sanity cap on the result.
public enum RetryAfter {
    public static func parse(_ header: String?, now: Date) -> TimeInterval? {
        guard let raw = header?.trimmingCharacters(in: .whitespaces), !raw.isEmpty else { return nil }
        if let secs = TimeInterval(raw) { return max(0, secs) }
        if let date = httpDate.date(from: raw) { return max(0, date.timeIntervalSince(now)) }
        return nil
    }

    private static let httpDate: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.timeZone = TimeZone(identifier: "GMT")
        f.dateFormat = "EEE, dd MMM yyyy HH:mm:ss zzz"   // RFC 7231 IMF-fixdate
        return f
    }()
}
