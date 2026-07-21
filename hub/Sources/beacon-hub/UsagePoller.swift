import Foundation
import Security
import BeaconHubKit

// Polls every usage-capable, usage-enabled provider from the registry every 45 s (and once at start),
// normalizes via UsageNormalizer, and emits per-provider results the app reduces (#108). Per-provider
// gating lives on the provider (Claude skips its oauth call while its statusline shim is fresh; most
// providers always poll). All provider secrets stay on the Mac; only computed pct/reset cross BLE
// (tech.md 9). Tokens are never logged. The unofficial endpoints sit behind UsageProvider (mockable).

// Result of one provider poll: the normalized windows (.unavailable on non-live) plus a classified
// outcome the reducer interprets (#108). .live carries the value; .transient/.terminal carry the
// reason and usage stays .unavailable (the reducer serves last-good on transient).
struct ProviderResult {
    let usage: ProviderUsage
    let outcome: ProviderOutcome
}

protocol UsageProvider {
    var label: String { get }                       // "Claude" / "Codex", used in error strings.
    func fetch(completion: @escaping (ProviderResult) -> Void)
}

final class UsagePoller {
    // Per-tick results keyed by provider id. A provider absent from the map was skipped this tick (its
    // gate said no / it is usage-disabled) -- the app treats absence as "no new observation" (#108).
    var onUpdate: (([String: ProviderResult]) -> Void)?

    private let providers: [AgentProvider]
    private let usageEnabled: (String) -> Bool
    private let interval: TimeInterval = 45
    // Exposed so the app's statusline-source precedence derives its freshness window from the SAME
    // interval as the poll gate -- they must never disagree.
    var pollInterval: TimeInterval { interval }
    private let backoff: TimeInterval = 300   // disconnected cadence (#64): no live device => no sub-min need.
    private var timer: DispatchSourceTimer?
    private let queue = DispatchQueue(label: "beacon.usage")

    private var connected = true
    private var lastPollAt = Date.distantPast

    init(providers: [AgentProvider], usageEnabled: @escaping (String) -> Bool) {
        self.providers = providers
        self.usageEnabled = usageEnabled
    }

    // AppDelegate -> poller, thread-safe. Drives the disconnected backoff cadence.
    func setDeviceConnected(_ b: Bool) { queue.async { [weak self] in self?.connected = b } }

    func start() {
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now(), repeating: interval, leeway: .seconds(5))   // #66 L6: coalesce wakeups.
        t.setEventHandler { [weak self] in self?.tick() }
        timer = t
        t.resume()
    }

    private func tick() {
        let now = Date()
        guard UsagePollDecision.shouldPoll(connected: connected,
                                           secondsSinceLastPoll: now.timeIntervalSince(lastPollAt),
                                           backoff: backoff) else { return }
        lastPollAt = now
        poll(now: now)
    }

    // Poll every usage-capable, usage-enabled provider whose gate allows it this tick, in parallel;
    // collect classified results and hand the map to the app on the main actor.
    private func poll(now: Date) {
        let group = DispatchGroup()
        let lock = NSLock()
        var results: [String: ProviderResult] = [:]
        for p in providers where p.descriptor.supportsUsage && usageEnabled(p.descriptor.id) {
            guard let source = p.usageSource, p.shouldPollUsage(now: now, interval: interval) else { continue }
            let id = p.descriptor.id
            group.enter()
            source.fetch { res in
                p.noteUsageOutcome(res.outcome)   // per-provider backoff (Claude oauth 429 curve, #108)
                lock.lock(); results[id] = res; lock.unlock()
                group.leave()
            }
        }
        group.notify(queue: queue) { [weak self] in
            let cb = self?.onUpdate
            DispatchQueue.main.async { cb?(results) }
        }
    }
}

// --- Claude: Keychain token -> oauth/usage ---

final class ClaudeUsageProvider: UsageProvider {
    let label = "Claude"
    private let session: URLSession
    // Cache the Keychain credential so we read (and prompt for) it ONCE per run, not every poll. macOS
    // prompts to release another app's credential on each SecItemCopyMatching; caching = one prompt.
    // Cleared on 401 or when expired per expiresAt, so a CLI-rotated token is re-read (one fresh
    // prompt only when actually needed).
    // NOTE: "Always Allow" only persists across rebuilds with a stable signing identity; ad-hoc
    // signing changes the cdhash each build, so a rebuild re-prompts once.
    private var cachedCredential: ClaudeCredential?
    private var lastReadAt: Date?
    private let rereadCooldown: TimeInterval = 300
    private let lock = NSLock()
    init(session: URLSession = .shared) { self.session = session }

    private func credential() -> ClaudeCredential? {
        lock.lock(); defer { lock.unlock() }
        let now = Date()
        if let c = cachedCredential, !c.isExpired(at: now) { return c }
        // Expired (or absent) cache: the CLI may have rotated the Keychain item, but re-read at most
        // once per cooldown -- each read can prompt the user (see cache note above).
        guard UsagePollDecision.shouldRereadCredential(
            secondsSinceLastRead: lastReadAt.map { now.timeIntervalSince($0) },
            cooldown: rereadCooldown)
        else { return cachedCredential }
        lastReadAt = now
        cachedCredential = Self.readCredential()
        return cachedCredential
    }
    // Also resets the cooldown: a 401 is a strong rotation signal worth one immediate re-read.
    private func invalidateToken() { lock.lock(); cachedCredential = nil; lastReadAt = nil; lock.unlock() }

    func fetch(completion: @escaping (ProviderResult) -> Void) {
        let now = Date()
        guard let cred = credential() else {
            completion(ProviderResult(usage: .unavailable,
                                      outcome: .terminal(reason: "Claude token missing - run claude login",
                                                         kind: .missingCredential)))
            return
        }
        // Expired on disk is NOT a logged-out state: only the `claude` CLI can run the (single-use,
        // rotating) refresh grant. Skip the doomed request and say what we are actually waiting for.
        if cred.isExpired(at: now) {
            completion(ProviderResult(usage: .unavailable,
                                      outcome: .terminal(reason: "Claude token stale - open Claude Code to refresh",
                                                         kind: .staleToken)))
            return
        }
        fetch(token: cred.accessToken, retryOn401: true, completion: completion)
    }

    // retryOn401: a bool not a counter => exactly one retry, structurally impossible to loop. On 401 the
    // public path (true) re-reads the CLI-rotated Keychain token and re-issues ONCE with retryOn401:false
    // iff the token actually changed; an unchanged token or a second 401 surfaces the terminal reason.
    private func fetch(token: String, retryOn401: Bool, completion: @escaping (ProviderResult) -> Void) {
        guard let url = URL(string: "https://api.anthropic.com/api/oauth/usage") else {
            completion(ProviderResult(usage: .unavailable, outcome: .terminal(reason: "Claude endpoint invalid", kind: .other)))
            return
        }
        var req = URLRequest(url: url)
        req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        req.setValue("oauth-2025-04-20", forHTTPHeaderField: "anthropic-beta")
        // Some Anthropic edges 429 requests without a UA/Accept. Best-effort; the statusline rate_limits
        // path (ClaudeCodeBridge) is the robust source if the oauth endpoint keeps 429-ing.
        req.setValue("claude-cli/1.0 (beacon-hub)", forHTTPHeaderField: "User-Agent")
        req.setValue("application/json", forHTTPHeaderField: "Accept")

        session.dataTask(with: req) { data, resp, err in
            if let err = err {
                completion(ProviderResult(usage: .unavailable, outcome: .transient(retryAfter: nil,
                    reason: "Claude network error: \(err.localizedDescription)")))
                return
            }
            let http = resp as? HTTPURLResponse
            let code = http?.statusCode ?? 0
            if code == 401 {
                self.invalidateToken()   // token rotated; re-read from Keychain.
                // Self-heal within this poll: the running `claude` CLI rotates the Keychain item before
                // expiry, so re-read once and retry iff the token changed. An unchanged token that 401s
                // before its expiresAt is genuinely rejected (revoked), not stale => re-login is right.
                if retryOn401, let fresh = self.credential(), fresh.accessToken != token {
                    self.fetch(token: fresh.accessToken, retryOn401: false, completion: completion)
                    return
                }
                completion(ProviderResult(usage: .unavailable,
                                          outcome: .terminal(reason: "Claude token expired - re-login", kind: .other)))
                return
            }
            guard code == 200, let data = data else {
                // 429 / 5xx / other: transient. Honor a server Retry-After (#108).
                let retryAfter = RetryAfter.parse(http?.value(forHTTPHeaderField: "Retry-After"), now: Date())
                completion(ProviderResult(usage: .unavailable, outcome: .transient(retryAfter: retryAfter,
                    reason: "Claude usage unavailable (HTTP \(code))")))
                return
            }
            guard let usage = UsageNormalizer.claude(data) else {
                // HTTP 200 but the body did not normalize => the unofficial endpoint's shape likely
                // drifted. Surface it distinctly so a schema change is visible, not a silent "--".
                completion(ProviderResult(usage: .unavailable,
                                          outcome: .terminal(reason: "Claude usage: unexpected response shape", kind: .other)))
                return
            }
            completion(ProviderResult(usage: usage, outcome: .live))
        }.resume()
    }

    // Keychain generic-password "Claude Code-credentials"; the JSON shape is parsed by ProviderCredentials.
    private static func readCredential() -> ClaudeCredential? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: "Claude Code-credentials",
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess,
              let data = item as? Data
        else { return nil }
        return ProviderCredentials.parseClaude(data)
    }
}

// --- Codex: ~/.codex/auth.json token -> wham/usage ---

final class CodexUsageProvider: UsageProvider {
    let label = "Codex"
    private let session: URLSession
    init(session: URLSession = .shared) { self.session = session }

    func fetch(completion: @escaping (ProviderResult) -> Void) {
        guard let creds = Self.credentials() else {
            completion(ProviderResult(usage: .unavailable,
                                      outcome: .terminal(reason: "Codex token missing - run codex login", kind: .other)))
            return
        }
        fetch(token: creds.accessToken, accountId: creds.accountId, retryOn401: true, completion: completion)
    }

    // retryOn401: a bool not a counter => exactly one retry, structurally impossible to loop. On 401 the
    // public path (true) re-reads CLI-rotated ~/.codex/auth.json and re-issues ONCE with retryOn401:false
    // iff access_token actually changed; an unchanged token or a second 401 surfaces the terminal reason.
    private func fetch(token: String, accountId: String, retryOn401: Bool,
                       completion: @escaping (ProviderResult) -> Void) {
        guard let url = URL(string: "https://chatgpt.com/backend-api/wham/usage") else {
            completion(ProviderResult(usage: .unavailable, outcome: .terminal(reason: "Codex endpoint invalid", kind: .other)))
            return
        }
        var req = URLRequest(url: url)
        req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        req.setValue(accountId, forHTTPHeaderField: "chatgpt-account-id")

        session.dataTask(with: req) { data, resp, err in
            if let err = err {
                completion(ProviderResult(usage: .unavailable, outcome: .transient(retryAfter: nil,
                    reason: "Codex network error: \(err.localizedDescription)")))
                return
            }
            let http = resp as? HTTPURLResponse
            let code = http?.statusCode ?? 0
            if code == 401 {
                // Self-heal within this poll: the running `codex` CLI rotates auth.json in place (bumps
                // last_refresh), so re-read once and retry iff access_token changed (else genuinely expired).
                if retryOn401, let fresh = Self.credentials(), fresh.accessToken != token {
                    self.fetch(token: fresh.accessToken, accountId: fresh.accountId,
                               retryOn401: false, completion: completion)
                    return
                }
                completion(ProviderResult(usage: .unavailable,
                                          outcome: .terminal(reason: "Codex token expired - re-login", kind: .other)))
                return
            }
            guard code == 200, let data = data else {
                let retryAfter = RetryAfter.parse(http?.value(forHTTPHeaderField: "Retry-After"), now: Date())
                completion(ProviderResult(usage: .unavailable, outcome: .transient(retryAfter: retryAfter,
                    reason: "Codex usage unavailable (HTTP \(code))")))
                return
            }
            guard let usage = UsageNormalizer.codex(data) else {
                // HTTP 200 but the body did not normalize => the unofficial endpoint's shape likely
                // drifted. Surface it distinctly so a schema change is visible, not a silent "--".
                completion(ProviderResult(usage: .unavailable,
                                          outcome: .terminal(reason: "Codex usage: unexpected response shape", kind: .other)))
                return
            }
            completion(ProviderResult(usage: usage, outcome: .live))
        }.resume()
    }

    // ~/.codex/auth.json; the JSON shape is parsed by ProviderCredentials.
    private static func credentials() -> (accessToken: String, accountId: String)? {
        let path = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".codex/auth.json")
        guard let data = try? Data(contentsOf: path) else { return nil }
        return ProviderCredentials.parseCodex(data)
    }
}
