import Foundation

// Pure parsers for the two providers' on-disk credential blobs, split out of UsagePoller so the JSON
// SHAPE is unit-testable. The privileged READ (Keychain for Claude, ~/.codex/auth.json for Codex) stays
// in the executable; what breaks when a CLI changes its blob is the shape, so isolate + test it here
// (mirrors UsageNormalizer's "expect breakage, isolate the shape"). expiresAt is read so an expired-on-disk
// token is reported as "stale, waiting for the CLI" instead of a misleading "re-login". refreshToken and
// refreshTokenExpiresAt are now parsed too, for the #132 source ladder: when the claude CLI is installed
// it remains the sole token owner (its refresh is single-use/rotating, so we delegate to it rather than
// race it); a hub-side direct OAuth grant only fires when the CLI is absent. OPENAI_API_KEY stays unused.
// An empty-string token is treated as absent (=> nil).
public struct ClaudeCredential: Equatable {
    public let accessToken: String
    public let expiresAt: Date?              // nil when absent/unparseable; treated as never-expired.
    public let refreshToken: String?         // nil when absent/empty.
    public let refreshTokenExpiresAt: Date?  // nil when absent/unparseable; treated as never-expired.

    public init(accessToken: String, expiresAt: Date?,
                refreshToken: String? = nil, refreshTokenExpiresAt: Date? = nil) {
        self.accessToken = accessToken
        self.expiresAt = expiresAt
        self.refreshToken = refreshToken
        self.refreshTokenExpiresAt = refreshTokenExpiresAt
    }

    public func isExpired(at now: Date) -> Bool {
        guard let expiresAt else { return false }
        return expiresAt <= now
    }

    // Whether a refresh is even worth attempting: a present, non-empty refresh token whose own expiry
    // (if any) has not passed. Used to gate the #132 source ladder before picking delegated vs. direct.
    public func refreshTokenAlive(at now: Date) -> Bool {
        guard let refreshToken, !refreshToken.isEmpty else { return false }
        guard let refreshTokenExpiresAt else { return true }
        return now < refreshTokenExpiresAt
    }
}

public enum ProviderCredentials {

    // Claude Keychain blob: { claudeAiOauth: { accessToken, expiresAt (epoch ms), refreshToken,
    // refreshTokenExpiresAt (epoch ms), ... } }.
    public static func parseClaude(_ blob: Data) -> ClaudeCredential? {
        guard let obj = try? JSONSerialization.jsonObject(with: blob) as? [String: Any],
              let oauth = obj["claudeAiOauth"] as? [String: Any],
              let token = oauth["accessToken"] as? String, !token.isEmpty
        else { return nil }
        let expiresAt = (oauth["expiresAt"] as? NSNumber)
            .map { Date(timeIntervalSince1970: $0.doubleValue / 1000) }
        let refreshToken = (oauth["refreshToken"] as? String).flatMap { $0.isEmpty ? nil : $0 }
        let refreshTokenExpiresAt = (oauth["refreshTokenExpiresAt"] as? NSNumber)
            .map { Date(timeIntervalSince1970: $0.doubleValue / 1000) }
        return ClaudeCredential(accessToken: token, expiresAt: expiresAt,
                                refreshToken: refreshToken, refreshTokenExpiresAt: refreshTokenExpiresAt)
    }

    // Codex ~/.codex/auth.json: { tokens: { access_token, account_id, ... } }.
    public static func parseCodex(_ json: Data) -> (accessToken: String, accountId: String)? {
        guard let obj = try? JSONSerialization.jsonObject(with: json) as? [String: Any],
              let tokens = obj["tokens"] as? [String: Any],
              let token = tokens["access_token"] as? String, !token.isEmpty,
              let account = tokens["account_id"] as? String, !account.isEmpty
        else { return nil }
        return (token, account)
    }
}
