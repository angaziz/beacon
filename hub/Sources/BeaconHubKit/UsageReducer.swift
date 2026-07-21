import Foundation

// Per-provider usage reliability reducer (#108). The hub has two failure-prone usage sources (the
// Claude oauth endpoint 429s; either endpoint can blip). Instead of blanking to "--" on every
// transient failure, we retain the last LIVE value and serve it as STALE until it is too old or its
// quota window resets. This is pure so the whole (priorState, outcome, now) -> (display, nextState)
// path is host-tested without URLSession/timers.

// Why a terminal fetch failed, for the abandonment demotion (#126). Only Claude's credential paths
// carry meaningful kinds; every other terminal is .other and never demotes.
public enum TerminalKind: Equatable {
    case missingCredential
    case staleToken
    case other
}

// How a fetch resolved. Providers classify the transport facts only they can observe.
public enum ProviderOutcome: Equatable {
    case live                                                 // HTTP 200 + normalized OK
    case transient(retryAfter: TimeInterval?, reason: String) // 429 / 5xx / network -- retain last-good
    case terminal(reason: String, kind: TerminalKind) // token missing/expired, shape drift
    case inactive(reason: String)                     // abandoned provider (#126): quiet .info, no red banner
}

// A menubar note. `.info` is muted (rate-limited, showing last value); `.error` is the red banner.
public struct UsageNote: Equatable {
    public enum Severity: Equatable { case info, error }
    public let severity: Severity
    public let text: String
    public init(severity: Severity, text: String) { self.severity = severity; self.text = text }
}

// Retained last-known-good for one provider, carried across ticks.
public struct ProviderRetention: Equatable {
    public var lastGood: ProviderUsage?   // last LIVE value (from either source), stale flag cleared.
    public var lastGoodAt: Date?
    public init(lastGood: ProviderUsage? = nil, lastGoodAt: Date? = nil) {
        self.lastGood = lastGood; self.lastGoodAt = lastGoodAt
    }
}

// What to show/send for one provider this tick.
public struct ProviderDisplay: Equatable {
    public var usage: ProviderUsage   // value to render/send, with .stale set when serving last-good.
    public var note: UsageNote?       // nil, or a menubar note.
    public init(usage: ProviderUsage, note: UsageNote?) { self.usage = usage; self.note = note }
}

public enum UsageReducer {
    // Apply one observation (from the statusline or the oauth/Codex poll) to a provider's retention.
    //  - .live     : store last-good, show it live, no note.
    //  - .transient: keep last-good; per-window, show the retained pct (stale) or "--" once expired;
    //                provider.stale = true iff any window is still served. If every window expired (or
    //                there was never a last-good) => "--" + the error reason.
    //  - .terminal : clear last-good (credential-identity safety: a re-login must not show the old
    //                account) => "--" + the actionable reason.
    //  - .inactive : like .terminal but a muted .info note (abandoned provider, #126).
    public static func reduceProvider(prior: ProviderRetention, outcome: ProviderOutcome,
                                      usage: ProviderUsage, now: Date, maxStale: TimeInterval,
                                      label: String) -> (next: ProviderRetention, display: ProviderDisplay) {
        switch outcome {
        case .live:
            var live = usage; live.stale = nil
            return (ProviderRetention(lastGood: live, lastGoodAt: now),
                    ProviderDisplay(usage: live, note: nil))

        case .terminal(let reason, _):
            return (ProviderRetention(lastGood: nil, lastGoodAt: nil),
                    ProviderDisplay(usage: .unavailable,
                                    note: UsageNote(severity: .error, text: reason)))

        case .inactive(let reason):
            return (ProviderRetention(lastGood: nil, lastGoodAt: nil),
                    ProviderDisplay(usage: .unavailable,
                                    note: UsageNote(severity: .info, text: reason)))

        case .transient(_, let reason):
            guard let good = prior.lastGood else {
                return (prior, ProviderDisplay(usage: .unavailable,
                                               note: UsageNote(severity: .error, text: reason)))
            }
            let h5Expired = UsagePollDecision.windowExpired(lastGoodAt: prior.lastGoodAt, now: now,
                                                            maxStale: maxStale, windowReset: good.h5.reset)
            let d7Expired = UsagePollDecision.windowExpired(lastGoodAt: prior.lastGoodAt, now: now,
                                                            maxStale: maxStale, windowReset: good.d7.reset)
            let h5 = h5Expired ? UsageWindow(pct: nil, reset: good.h5.reset) : good.h5
            let d7 = d7Expired ? UsageWindow(pct: nil, reset: good.d7.reset) : good.d7
            let anyServed = !h5Expired || !d7Expired
            let shown = ProviderUsage(h5: h5, d7: d7, stale: anyServed ? true : nil)
            let note = anyServed
                ? UsageNote(severity: .info, text: "\(label) rate-limited - last value \(hhmm(prior.lastGoodAt))")
                : UsageNote(severity: .error, text: reason)
            return (prior, ProviderDisplay(usage: shown, note: note))
        }
    }

    // A usage-disabled provider must not surface its (possibly frozen) note (#126): filter by the live
    // usage-enabled predicate before assembling the menubar note list, in registration order.
    public static func visibleNotes(order: [String], notes: [String: UsageNote],
                                    enabled: (String) -> Bool) -> [UsageNote] {
        order.compactMap { enabled($0) ? notes[$0] : nil }
    }

    // Fixed at lastGoodAt (not wall-clock) so the note text does not churn every minute (#108 two-channel
    // gate). Hoisted formatter (one instance), matching UsageNormalizer's pattern.
    private static let timeFmt: DateFormatter = {
        let f = DateFormatter(); f.dateFormat = "h:mm a"; return f
    }()
    private static func hhmm(_ date: Date?) -> String {
        guard let date else { return "--" }
        return timeFmt.string(from: date)
    }
}
