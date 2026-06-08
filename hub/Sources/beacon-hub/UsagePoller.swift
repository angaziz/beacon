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
        guard let url = URL(string: "https://api.anthropic.com/api/oauth/usage") else {
            completion(ProviderResult(usage: .unavailable, reason: "Claude endpoint invalid"))
            return
        }
        var req = URLRequest(url: url)
        req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        req.setValue("oauth-2025-04-20", forHTTPHeaderField: "anthropic-beta")

        session.dataTask(with: req) { data, resp, err in
            if let err = err {
                completion(ProviderResult(usage: .unavailable, reason: "Claude network error: \(err.localizedDescription)"))
                return
            }
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            if code == 401 {
                self.invalidateToken()   // token rotated; re-read from Keychain next poll
                // TODO(P2-0): retry once via stored refresh token (D2). Refresh endpoint + field
                // names are unverified until the P2-0 fixtures land. Happy path only for now.
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

    // Keychain generic-password "Claude Code-credentials"; JSON at claudeAiOauth.accessToken.
    private static func accessToken() -> String? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: "Claude Code-credentials",
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess,
              let data = item as? Data,
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let oauth = obj["claudeAiOauth"] as? [String: Any],
              let tok = oauth["accessToken"] as? String, !tok.isEmpty
        else { return nil }
        return tok
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
        guard let url = URL(string: "https://chatgpt.com/backend-api/wham/usage") else {
            completion(ProviderResult(usage: .unavailable, reason: "Codex endpoint invalid"))
            return
        }
        var req = URLRequest(url: url)
        req.setValue("Bearer \(creds.token)", forHTTPHeaderField: "Authorization")
        req.setValue(creds.accountId, forHTTPHeaderField: "chatgpt-account-id")

        session.dataTask(with: req) { data, resp, err in
            if let err = err {
                completion(ProviderResult(usage: .unavailable, reason: "Codex network error: \(err.localizedDescription)"))
                return
            }
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            if code == 401 {
                // TODO(P2-0): retry once via stored refresh token (D2) + the local rollout fallback
                // (D1: ~/.codex/sessions/**/rollout-*.jsonl rate_limits). Field names land at P2-0.
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

    private static func credentials() -> (token: String, accountId: String)? {
        let path = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".codex/auth.json")
        guard let data = try? Data(contentsOf: path),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let tokens = obj["tokens"] as? [String: Any],
              let token = tokens["access_token"] as? String, !token.isEmpty,
              let account = tokens["account_id"] as? String, !account.isEmpty
        else { return nil }
        return (token, account)
    }
}
