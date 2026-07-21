import Foundation

// Pure decisions for the Claude "source ladder" (#132): when the on-disk token is expired, decide
// HOW to refresh it (or whether to bother) without touching Keychain/subprocess/network, so the
// ladder logic is table-testable like UsagePollDecision.
public enum ClaudeRefreshPath: Equatable {
    case delegated  // ask the claude CLI to do it (re-read its Keychain item afterward)
    case direct     // hit the Anthropic OAuth token endpoint ourselves
    case none       // nothing usable to refresh with
}

public enum ClaudeRefreshDecision {

    // CLI-ownership rule (CodexBar-style): Anthropic refresh tokens are single-use/rotating, so
    // whoever calls the token endpoint invalidates the token for everyone else. When the claude CLI
    // is installed it is the token's owner -- a hub-side direct grant would rotate the token out from
    // under it and silently log the user out of Claude Code. So: CLI present => always delegate to it;
    // CLI absent => the hub is the only actor left, so it may refresh directly. No usable refresh
    // token (missing/empty/expired) => neither path can work.
    public static func path(cliAvailable: Bool, refreshTokenAlive: Bool) -> ClaudeRefreshPath {
        guard refreshTokenAlive else { return .none }
        return cliAvailable ? .delegated : .direct
    }

    // Anti-thrash cooldown gate: a refresh attempt (delegated subprocess spawn or direct network call)
    // is not free and a failing token doesn't get less expired by retrying every tick, so throttle
    // retries to `cooldown`. nil (never attempted) always fires so the very first expiry is handled
    // immediately.
    public static func shouldAttempt(secondsSinceLastAttempt: TimeInterval?, cooldown: TimeInterval) -> Bool {
        guard let since = secondsSinceLastAttempt else { return true }
        return since >= cooldown
    }

    public struct RefreshedToken: Equatable {
        public let accessToken: String
        public let refreshToken: String?
        public let expiresAt: Date?

        public init(accessToken: String, refreshToken: String?, expiresAt: Date?) {
            self.accessToken = accessToken
            self.refreshToken = refreshToken
            self.expiresAt = expiresAt
        }
    }

    // Isolates the Anthropic OAuth token-endpoint response shape (mirrors ProviderCredentials'
    // "expect breakage, isolate the shape"): the response is out of our control, so tolerate a
    // missing/non-numeric expires_in and a missing/empty refresh_token (rotation isn't guaranteed
    // every call) -- only a missing/empty access_token makes the response unusable.
    public static func parseRefreshResponse(_ data: Data, now: Date) -> RefreshedToken? {
        guard let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let accessToken = obj["access_token"] as? String, !accessToken.isEmpty
        else { return nil }
        let refreshToken = (obj["refresh_token"] as? String).flatMap { $0.isEmpty ? nil : $0 }
        let expiresAt = (obj["expires_in"] as? NSNumber)
            .map { now.addingTimeInterval($0.doubleValue) }
        return RefreshedToken(accessToken: accessToken, refreshToken: refreshToken, expiresAt: expiresAt)
    }
}
