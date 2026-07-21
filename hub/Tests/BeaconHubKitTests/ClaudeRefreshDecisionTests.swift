import XCTest
@testable import BeaconHubKit

final class ClaudeRefreshDecisionTests: XCTestCase {

    func testPath() {
        struct Case { let name: String; let cliAvailable: Bool; let refreshTokenAlive: Bool; let want: ClaudeRefreshPath }
        let cases: [Case] = [
            Case(name: "cli present, token alive => delegated", cliAvailable: true, refreshTokenAlive: true,
                 want: .delegated),
            Case(name: "cli present, token dead => none", cliAvailable: true, refreshTokenAlive: false,
                 want: .none),
            Case(name: "cli absent, token alive => direct", cliAvailable: false, refreshTokenAlive: true,
                 want: .direct),
            Case(name: "cli absent, token dead => none", cliAvailable: false, refreshTokenAlive: false,
                 want: .none),
        ]
        for c in cases {
            XCTAssertEqual(
                ClaudeRefreshDecision.path(cliAvailable: c.cliAvailable, refreshTokenAlive: c.refreshTokenAlive),
                c.want, "case: \(c.name)")
        }
    }

    func testShouldAttempt() {
        struct Case { let name: String; let secondsSinceLastAttempt: TimeInterval?; let cooldown: TimeInterval; let want: Bool }
        let cases: [Case] = [
            Case(name: "never attempted => true", secondsSinceLastAttempt: nil, cooldown: 60, want: true),
            Case(name: "below cooldown => false", secondsSinceLastAttempt: 30, cooldown: 60, want: false),
            Case(name: "at boundary => true", secondsSinceLastAttempt: 60, cooldown: 60, want: true),
            Case(name: "above cooldown => true", secondsSinceLastAttempt: 90, cooldown: 60, want: true),
        ]
        for c in cases {
            XCTAssertEqual(
                ClaudeRefreshDecision.shouldAttempt(secondsSinceLastAttempt: c.secondsSinceLastAttempt,
                                                    cooldown: c.cooldown),
                c.want, "case: \(c.name)")
        }
    }

    func testParseRefreshResponse() {
        let now = Date(timeIntervalSince1970: 1_000_000)
        struct Case { let name: String; let json: String; let want: ClaudeRefreshDecision.RefreshedToken? }
        let cases: [Case] = [
            Case(name: "full response",
                 json: #"{"access_token":"at","refresh_token":"rt","expires_in":3600}"#,
                 want: ClaudeRefreshDecision.RefreshedToken(accessToken: "at", refreshToken: "rt",
                                                            expiresAt: now.addingTimeInterval(3600))),
            Case(name: "no refresh_token",
                 json: #"{"access_token":"at","expires_in":3600}"#,
                 want: ClaudeRefreshDecision.RefreshedToken(accessToken: "at", refreshToken: nil,
                                                            expiresAt: now.addingTimeInterval(3600))),
            Case(name: "empty refresh_token => nil field",
                 json: #"{"access_token":"at","refresh_token":"","expires_in":3600}"#,
                 want: ClaudeRefreshDecision.RefreshedToken(accessToken: "at", refreshToken: nil,
                                                            expiresAt: now.addingTimeInterval(3600))),
            Case(name: "expires_in as Int",
                 json: #"{"access_token":"at","expires_in":3600}"#,
                 want: ClaudeRefreshDecision.RefreshedToken(accessToken: "at", refreshToken: nil,
                                                            expiresAt: now.addingTimeInterval(3600))),
            Case(name: "expires_in as Double",
                 json: #"{"access_token":"at","expires_in":3600.5}"#,
                 want: ClaudeRefreshDecision.RefreshedToken(accessToken: "at", refreshToken: nil,
                                                            expiresAt: now.addingTimeInterval(3600.5))),
            Case(name: "missing expires_in => nil expiresAt",
                 json: #"{"access_token":"at"}"#,
                 want: ClaudeRefreshDecision.RefreshedToken(accessToken: "at", refreshToken: nil, expiresAt: nil)),
            Case(name: "empty access_token => nil",
                 json: #"{"access_token":""}"#, want: nil),
            Case(name: "missing access_token => nil",
                 json: #"{"expires_in":3600}"#, want: nil),
            Case(name: "malformed json => nil", json: "not json", want: nil),
        ]
        for c in cases {
            XCTAssertEqual(
                ClaudeRefreshDecision.parseRefreshResponse(Data(c.json.utf8), now: now), c.want,
                "case: \(c.name)")
        }
    }
}
