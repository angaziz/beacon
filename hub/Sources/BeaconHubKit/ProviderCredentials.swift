import Foundation

// Pure parsers for the two providers' on-disk credential blobs, split out of UsagePoller so the JSON
// SHAPE is unit-testable. The privileged READ (Keychain for Claude, ~/.codex/auth.json for Codex) stays
// in the executable; what breaks when a CLI changes its blob is the shape, so isolate + test it here
// (mirrors UsageNormalizer's "expect breakage, isolate the shape"). We parse ONLY the access fields the
// poller actually uses: refreshToken/expiresAt/OPENAI_API_KEY are unused (no network grant) and parsing
// them would be speculative. An empty-string token is treated as absent (=> nil).
public enum ProviderCredentials {

    // Claude Keychain blob: { claudeAiOauth: { accessToken, ... } }.
    public static func parseClaude(_ blob: Data) -> String? {
        guard let obj = try? JSONSerialization.jsonObject(with: blob) as? [String: Any],
              let oauth = obj["claudeAiOauth"] as? [String: Any],
              let token = oauth["accessToken"] as? String, !token.isEmpty
        else { return nil }
        return token
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
