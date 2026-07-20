import Foundation

// Pure poll-gating decisions for UsagePoller (#64), split out so the load-bearing "should we poll?"
// rules are table-testable without URLSession/timers.
public enum UsagePollDecision {
    // Poll at the base cadence while a device is connected (a live device wants 30-60s-fresh usage);
    // back off to a longer cadence while disconnected, where no consumer needs sub-minute freshness.
    public static func shouldPoll(connected: Bool, secondsSinceLastPoll: TimeInterval,
                                  backoff: TimeInterval) -> Bool {
        connected || secondsSinceLastPoll >= backoff
    }

    // Single source of the statusline freshness window (#93): a statusline value counts as live for 2x
    // the poll interval. Both the Claude poll-gate (skip the Keychain while fresh) and the display
    // fallback (prefer the statusline value while fresh, else fall back to the poller) derive from this,
    // so they can never disagree. nil age (never seen) => never fresh.
    public static func statuslineFresh(age: TimeInterval?, interval: TimeInterval) -> Bool {
        guard let age else { return false }
        return age < 2 * interval
    }

    // The Claude oauth/usage endpoint (now best-effort, often 429) is wasted work while the Claude Code
    // statusline shim is feeding usage -- AppDelegate prefers the statusline value. Skip the poll exactly
    // while the statusline is fresh; nil age (never seen) or a stale age => poll as fallback.
    public static func shouldPollClaude(statuslineAge: TimeInterval?, interval: TimeInterval) -> Bool {
        !statuslineFresh(age: statuslineAge, interval: interval)
    }

    // Exponential backoff for the Claude oauth endpoint after consecutive transient failures (#108).
    // consecutiveFails 0 => 0 (no backoff); 1 => base; 2 => 2*base; ... clamped to cap. Pure: jitter and
    // any server-directed Retry-After are composed in pollDelay so this stays deterministic for tests.
    public static func backoff(consecutiveFails: Int, base: TimeInterval, cap: TimeInterval) -> TimeInterval {
        guard consecutiveFails > 0 else { return 0 }
        let scaled = base * pow(2, Double(consecutiveFails - 1))
        return min(scaled, cap)
    }

    // A server-directed Retry-After is honored up to its own (larger) sanity cap, NOT the exponential
    // cap -- an explicit "wait 1h" must be respected. Absurd/negative values are rejected (#108).
    public static func sanitizedRetryAfter(_ retryAfter: TimeInterval?, sanityCap: TimeInterval) -> TimeInterval {
        guard let r = retryAfter, r > 0 else { return 0 }
        return min(r, sanityCap)
    }

    // Final delay until the next allowed Claude oauth poll: never earlier than the (sanitized)
    // server-directed Retry-After floor, else our jittered exponential backoff. jitterFraction is passed
    // in (e.g. Double.random(in: -0.2...0.2)) so this function stays deterministic; pass 0 in tests.
    // Floor wins so negative jitter can never schedule a poll before the server-mandated cooldown.
    public static func pollDelay(consecutiveFails: Int, retryAfter: TimeInterval?,
                                 base: TimeInterval, cap: TimeInterval,
                                 retryAfterSanityCap: TimeInterval, jitterFraction: Double) -> TimeInterval {
        let floor = sanitizedRetryAfter(retryAfter, sanityCap: retryAfterSanityCap)
        let jittered = backoff(consecutiveFails: consecutiveFails, base: base, cap: cap) * (1 + jitterFraction)
        return max(floor, jittered)
    }

    // A retained window is dropped (=> "--") once it is older than maxStale OR its own quota window has
    // reset (the percentage would be semantically wrong post-rollover, not merely old). Per-window:
    // h5/d7 reset independently. nil lastGoodAt => expired. now/reset are epoch-comparable.
    public static func windowExpired(lastGoodAt: Date?, now: Date,
                                     maxStale: TimeInterval, windowReset: Int) -> Bool {
        guard let at = lastGoodAt else { return true }
        if now.timeIntervalSince(at) > maxStale { return true }
        if windowReset > 0, now.timeIntervalSince1970 >= Double(windowReset) { return true }
        return false
    }

    // Gate Keychain re-reads while the stored Claude token sits expired (waiting for the CLI to rotate
    // it): each SecItemCopyMatching can prompt the user unless they chose "Always Allow", so re-reading
    // every 45s tick would nag every 45s. nil = never read.
    public static func shouldRereadCredential(secondsSinceLastRead: TimeInterval?,
                                              cooldown: TimeInterval) -> Bool {
        guard let since = secondsSinceLastRead else { return true }
        return since >= cooldown
    }

    // Abandonment demotion (#126): a credential expired past `threshold` with no statusline POST inside
    // `threshold`. A missing credential is NOT demoted -- no durable first-seen timestamp exists to prove
    // >= threshold, and it is indistinguishable from a never-logged-in new user who needs the actionable
    // "run claude login" message. nil statuslineAge (never seen this launch) counts as "no activity".
    public static func providerInactive(kind: TerminalKind, statuslineAge: TimeInterval?,
                                        threshold: TimeInterval) -> Bool {
        let expiredLongEnough: Bool
        switch kind {
        case .staleToken(let expiredFor):   expiredLongEnough = expiredFor >= threshold
        case .missingCredential, .other:    expiredLongEnough = false
        }
        guard expiredLongEnough else { return false }
        return statuslineAge.map { $0 >= threshold } ?? true
    }
}
