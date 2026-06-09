import Foundation
import Security
import BeaconHubKit

// Polls Claude + Codex usage every 45 s (and once at start), normalizes via UsageNormalizer, and
// emits the merged Usage plus any human-readable error strings for the menubar. All provider secrets
// (Keychain / ~/.codex) stay on the Mac; only computed pct/reset cross BLE (tech.md 9). Tokens are
// never logged. The two unofficial endpoints sit behind UsageProvider so they can be swapped/mocked.

// Result of one provider poll: the normalized windows (or .unavailable) plus an optional menubar
// reason (nil on success). On failure usage is .unavailable AND reason is set (D2).
struct ProviderResult {
    let usage: ProviderUsage
    let reason: String?   // e.g. "Claude token expired - re-login"; nil on success.
}

protocol UsageProvider {
    var label: String { get }                       // "Claude" / "Codex", used in error strings.
    func fetch(completion: @escaping (ProviderResult) -> Void)
}

final class UsagePoller {
    var onUpdate: ((Usage, [String]) -> Void)?

    private let claude: UsageProvider
    private let codex: UsageProvider
    private let interval: TimeInterval = 45
    private let session: URLSession
    private var timer: DispatchSourceTimer?
    private let queue = DispatchQueue(label: "beacon.usage")

    init(claude: UsageProvider = ClaudeUsageProvider(),
         codex: UsageProvider = CodexUsageProvider()) {
        self.claude = claude
        self.codex = codex
        let cfg = URLSessionConfiguration.ephemeral
        cfg.timeoutIntervalForRequest = 15
        self.session = URLSession(configuration: cfg)
    }

    func start() {
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now(), repeating: interval)
        t.setEventHandler { [weak self] in self?.poll() }
        timer = t
        t.resume()
    }

    private func poll() {
        let group = DispatchGroup()
        var claudeRes: ProviderResult?
        var codexRes: ProviderResult?

        group.enter()
        claude.fetch { claudeRes = $0; group.leave() }
        group.enter()
        codex.fetch { codexRes = $0; group.leave() }

        group.notify(queue: queue) { [weak self] in
            guard let self else { return }
            let c = claudeRes ?? ProviderResult(usage: .unavailable, reason: "Claude unavailable")
            let x = codexRes ?? ProviderResult(usage: .unavailable, reason: "Codex unavailable")
            let usage = Usage(claude: c.usage, codex: x.usage)
            let errors = [c.reason, x.reason].compactMap { $0 }
            let cb = self.onUpdate
            DispatchQueue.main.async { cb?(usage, errors) }
        }
    }
}

// --- Claude: Keychain token -> oauth/usage ---

final class ClaudeUsageProvider: UsageProvider {
    let label = "Claude"
    private let session: URLSession
    // Cache the Keychain token so we read (and prompt for) it ONCE per run, not every poll. macOS
    // prompts to release another app's credential on each SecItemCopyMatching; caching = one prompt.
    // Cleared on 401 so a rotated token is re-read (one fresh prompt only when actually needed).
    // NOTE: "Always Allow" only persists across rebuilds with a stable signing identity; ad-hoc
    // signing changes the cdhash each build, so a rebuild re-prompts once.
    private var cachedToken: String?
    private let lock = NSLock()
    init(session: URLSession = .shared) { self.session = session }

    private func token() -> String? {
        lock.lock(); defer { lock.unlock() }
        if let t = cachedToken { return t }
        cachedToken = Self.accessToken()
        return cachedToken
    }
    private func invalidateToken() { lock.lock(); cachedToken = nil; lock.unlock() }

    func fetch(completion: @escaping (ProviderResult) -> Void) {
        guard let token = token() else {
            completion(ProviderResult(usage: .unavailable, reason: "Claude token missing - run claude login"))
            return
        }
        fetch(token: token, retryOn401: true, completion: completion)
    }

    // retryOn401: a bool not a counter => exactly one retry, structurally impossible to loop. On 401 the
    // public path (true) re-reads the CLI-rotated Keychain token and re-issues ONCE with retryOn401:false
    // iff the token actually changed; an unchanged token or a second 401 surfaces the terminal reason.
    private func fetch(token: String, retryOn401: Bool, completion: @escaping (ProviderResult) -> Void) {
        guard let url = URL(string: "https://api.anthropic.com/api/oauth/usage") else {
            completion(ProviderResult(usage: .unavailable, reason: "Claude endpoint invalid"))
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
                completion(ProviderResult(usage: .unavailable, reason: "Claude network error: \(err.localizedDescription)"))
                return
            }
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            if code == 401 {
                self.invalidateToken()   // token rotated; re-read from Keychain.
                // Self-heal within this poll: the running `claude` CLI rotates the Keychain item before
                // expiry, so re-read once and retry iff the token changed (else it is genuinely expired).
                if retryOn401, let fresh = self.token(), fresh != token {
                    self.fetch(token: fresh, retryOn401: false, completion: completion)
                    return
                }
                completion(ProviderResult(usage: .unavailable, reason: "Claude token expired - re-login"))
                return
            }
            guard code == 200, let data = data, let usage = UsageNormalizer.claude(data) else {
                completion(ProviderResult(usage: .unavailable, reason: "Claude usage unavailable (HTTP \(code))"))
                return
            }
            completion(ProviderResult(usage: usage, reason: nil))
        }.resume()
    }

    // Keychain generic-password "Claude Code-credentials"; the JSON shape is parsed by ProviderCredentials.
    private static func accessToken() -> String? {
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
            completion(ProviderResult(usage: .unavailable, reason: "Codex token missing - run codex login"))
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
            completion(ProviderResult(usage: .unavailable, reason: "Codex endpoint invalid"))
            return
        }
        var req = URLRequest(url: url)
        req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        req.setValue(accountId, forHTTPHeaderField: "chatgpt-account-id")

        session.dataTask(with: req) { data, resp, err in
            if let err = err {
                completion(ProviderResult(usage: .unavailable, reason: "Codex network error: \(err.localizedDescription)"))
                return
            }
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            if code == 401 {
                // Self-heal within this poll: the running `codex` CLI rotates auth.json in place (bumps
                // last_refresh), so re-read once and retry iff access_token changed (else genuinely expired).
                if retryOn401, let fresh = Self.credentials(), fresh.accessToken != token {
                    self.fetch(token: fresh.accessToken, accountId: fresh.accountId,
                               retryOn401: false, completion: completion)
                    return
                }
                completion(ProviderResult(usage: .unavailable, reason: "Codex token expired - re-login"))
                return
            }
            guard code == 200, let data = data, let usage = UsageNormalizer.codex(data) else {
                completion(ProviderResult(usage: .unavailable, reason: "Codex usage unavailable (HTTP \(code))"))
                return
            }
            completion(ProviderResult(usage: usage, reason: nil))
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
