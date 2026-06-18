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

    // Gate Keychain re-reads while the stored Claude token sits expired (waiting for the CLI to rotate
    // it): each SecItemCopyMatching can prompt the user unless they chose "Always Allow", so re-reading
    // every 45s tick would nag every 45s. nil = never read.
    public static func shouldRereadCredential(secondsSinceLastRead: TimeInterval?,
                                              cooldown: TimeInterval) -> Bool {
        guard let since = secondsSinceLastRead else { return true }
        return since >= cooldown
    }
}
