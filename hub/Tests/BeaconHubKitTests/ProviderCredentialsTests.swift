import XCTest
@testable import BeaconHubKit

final class ProviderCredentialsTests: XCTestCase {

    func testParseClaude() {
        struct Case { let name: String; let json: String; let want: String? }
        let cases: [Case] = [
            Case(name: "valid", json: #"{"claudeAiOauth":{"accessToken":"tok-123","refreshToken":"r"}}"#, want: "tok-123"),
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
