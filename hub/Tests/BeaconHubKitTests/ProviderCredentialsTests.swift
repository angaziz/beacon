import XCTest
@testable import BeaconHubKit

final class ProviderCredentialsTests: XCTestCase {

    func testParseClaude() {
        struct Case { let name: String; let json: String; let want: ClaudeCredential? }
        let cases: [Case] = [
            Case(name: "valid, no expiry", json: #"{"claudeAiOauth":{"accessToken":"tok-123","refreshToken":"r"}}"#,
                 want: ClaudeCredential(accessToken: "tok-123", expiresAt: nil)),
            Case(name: "valid with expiry (epoch ms)",
                 json: #"{"claudeAiOauth":{"accessToken":"tok-123","expiresAt":1773751428445}}"#,
                 want: ClaudeCredential(accessToken: "tok-123",
                                        expiresAt: Date(timeIntervalSince1970: 1_773_751_428.445))),
            Case(name: "non-numeric expiresAt tolerated",
                 json: #"{"claudeAiOauth":{"accessToken":"tok-123","expiresAt":"soon"}}"#,
                 want: ClaudeCredential(accessToken: "tok-123", expiresAt: nil)),
            Case(name: "malformed json", json: "not json", want: nil),
            Case(name: "missing claudeAiOauth", json: #"{"other":{"accessToken":"t"}}"#, want: nil),
            Case(name: "missing accessToken", json: #"{"claudeAiOauth":{"refreshToken":"r"}}"#, want: nil),
            Case(name: "empty token => absent", json: #"{"claudeAiOauth":{"accessToken":""}}"#, want: nil),
        ]
        for c in cases {
            XCTAssertEqual(
                ProviderCredentials.parseClaude(Data(c.json.utf8)), c.want, "case: \(c.name)")
        }
    }

    func testClaudeCredentialIsExpired() {
        let now = Date(timeIntervalSince1970: 1_000_000)
        struct Case { let name: String; let expiresAt: Date?; let want: Bool }
        let cases: [Case] = [
            Case(name: "no expiry => never stale", expiresAt: nil, want: false),
            Case(name: "future expiry", expiresAt: now.addingTimeInterval(60), want: false),
            Case(name: "past expiry", expiresAt: now.addingTimeInterval(-60), want: true),
            Case(name: "exactly now => expired", expiresAt: now, want: true),
        ]
        for c in cases {
            let cred = ClaudeCredential(accessToken: "t", expiresAt: c.expiresAt)
            XCTAssertEqual(cred.isExpired(at: now), c.want, "case: \(c.name)")
        }
    }

    func testParseCodex() {
        struct Case { let name: String; let json: String; let want: (String, String)? }
        let cases: [Case] = [
            Case(name: "valid", json: #"{"tokens":{"access_token":"at","account_id":"acc"}}"#, want: ("at", "acc")),
            Case(name: "malformed json", json: "{", want: nil),
            Case(name: "missing tokens", json: #"{"OPENAI_API_KEY":"k"}"#, want: nil),
            Case(name: "missing access_token", json: #"{"tokens":{"account_id":"acc"}}"#, want: nil),
            Case(name: "missing account_id", json: #"{"tokens":{"access_token":"at"}}"#, want: nil),
            Case(name: "empty access_token => absent", json: #"{"tokens":{"access_token":"","account_id":"acc"}}"#, want: nil),
            Case(name: "empty account_id => absent", json: #"{"tokens":{"access_token":"at","account_id":""}}"#, want: nil),
        ]
        for c in cases {
            let got = ProviderCredentials.parseCodex(Data(c.json.utf8))
            switch (got, c.want) {
            case let (g?, w?):
                XCTAssertEqual(g.accessToken, w.0, "case: \(c.name)")
                XCTAssertEqual(g.accountId, w.1, "case: \(c.name)")
            case (nil, nil):
                break
            default:
                XCTFail("case: \(c.name) -- got \(String(describing: got)), want \(String(describing: c.want))")
            }
        }
    }
}
