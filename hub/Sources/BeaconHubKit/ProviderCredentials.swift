import Foundation

// Pure parsers for the two providers' on-disk credential blobs, split out of UsagePoller so the JSON
// SHAPE is unit-testable. The privileged READ (Keychain for Claude, ~/.codex/auth.json for Codex) stays
// in the executable; what breaks when a CLI changes its blob is the shape, so isolate + test it here
// (mirrors UsageNormalizer's "expect breakage, isolate the shape"). We parse ONLY the fields the poller
// actually uses: refreshToken/OPENAI_API_KEY are unused (no network grant — Anthropic refresh tokens are
// single-use, so a hub-side grant would race the CLI and break its login). expiresAt is read so an
// expired-on-disk token is reported as "stale, waiting for the CLI" instead of a misleading "re-login".
// An empty-string token is treated as absent (=> nil).
public struct ClaudeCredential: Equatable {
    public let accessToken: String
    public let expiresAt: Date?   // nil when absent/unparseable; treated as never-expired.

    public init(accessToken: String, expiresAt: Date?) {
        self.accessToken = accessToken
        self.expiresAt = expiresAt
    }

    public func isExpired(at now: Date) -> Bool {
        guard let expiresAt else { return false }
        return expiresAt <= now
    }
}

public enum ProviderCredentials {

    // Claude Keychain blob: { claudeAiOauth: { accessToken, expiresAt (epoch ms), ... } }.
    public static func parseClaude(_ blob: Data) -> ClaudeCredential? {
        guard let obj = try? JSONSerialization.jsonObject(with: blob) as? [String: Any],
              let oauth = obj["claudeAiOauth"] as? [String: Any],
              let token = oauth["accessToken"] as? String, !token.isEmpty
        else { return nil }
        let expiresAt = (oauth["expiresAt"] as? NSNumber)
            .map { Date(timeIntervalSince1970: $0.doubleValue / 1000) }
        return ClaudeCredential(accessToken: token, expiresAt: expiresAt)
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
