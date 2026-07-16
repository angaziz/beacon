import XCTest
@testable import BeaconHubKit

final class UsageNormalizerEdgeCaseTests: XCTestCase {

    // MARK: - Claude

    func testClaudeHappyPath() {
        let json = """
        {
            "five_hour": {"utilization": 24.0, "resets_at": "2026-06-11T12:00:00Z"},
            "seven_day": {"utilization": 50.5, "resets_at": "2026-06-15T00:00:00Z"}
        }
        """.data(using: .utf8)!
        let result = UsageNormalizer.claude(json)
        XCTAssertNotNil(result)
        XCTAssertEqual(result?.h5.pct, 24)
        XCTAssertEqual(result?.d7.pct, 51)  // 50.5 rounds to 51
        XCTAssertGreaterThan(result!.h5.reset, 0)
        XCTAssertGreaterThan(result!.d7.reset, 0)
    }

    func testClaudeIntegerUtilization() {
        let json = """
        {
            "five_hour": {"utilization": 0, "resets_at": 1717600000},
            "seven_day": {"utilization": 100, "resets_at": 1717800000}
        }
        """.data(using: .utf8)!
        let result = UsageNormalizer.claude(json)
        XCTAssertNotNil(result)
        XCTAssertEqual(result?.h5.pct, 0)
        XCTAssertEqual(result?.d7.pct, 100)
        XCTAssertEqual(result?.h5.reset, 1717600000)
        XCTAssertEqual(result?.d7.reset, 1717800000)
    }

    func testClaudeStringUtilization() {
        let json = """
        {
            "five_hour": {"utilization": "33.7", "resets_at": 1000},
            "seven_day": {"utilization": "99.9", "resets_at": 2000}
        }
        """.data(using: .utf8)!
        let result = UsageNormalizer.claude(json)
        XCTAssertNotNil(result)
        XCTAssertEqual(result?.h5.pct, 34)  // 33.7 rounds to 34
        XCTAssertEqual(result?.d7.pct, 100) // 99.9 rounds to 100
    }

    func testClaudeClampsAbove100() {
        let json = """
        {
            "five_hour": {"utilization": 150.0, "resets_at": 0},
            "seven_day": {"utilization": -5.0, "resets_at": 0}
        }
        """.data(using: .utf8)!
        let result = UsageNormalizer.claude(json)
        XCTAssertNotNil(result)
        XCTAssertEqual(result?.h5.pct, 100) // clamped to 100
        XCTAssertEqual(result?.d7.pct, 0)   // clamped to 0
    }

    func testClaudeNilPctWhenMissing() {
        let json = """
        {
            "five_hour": {"resets_at": 1000},
            "seven_day": {"resets_at": 2000}
        }
        """.data(using: .utf8)!
        let result = UsageNormalizer.claude(json)
        XCTAssertNotNil(result)
        XCTAssertNil(result?.h5.pct)
        XCTAssertNil(result?.d7.pct)
    }

    func testClaudeISOWithFractionalSeconds() {
        let json = """
        {
            "five_hour": {"utilization": 10, "resets_at": "2026-06-11T12:30:45.123Z"},
            "seven_day": {"utilization": 20, "resets_at": "2026-06-15T00:00:00.000Z"}
        }
        """.data(using: .utf8)!
        let result = UsageNormalizer.claude(json)
        XCTAssertNotNil(result)
        XCTAssertGreaterThan(result!.h5.reset, 0)
        XCTAssertGreaterThan(result!.d7.reset, 0)
    }

    func testClaudeResetAsStringEpoch() {
        let json = """
        {
            "five_hour": {"utilization": 5, "resets_at": "1717600000"},
            "seven_day": {"utilization": 10, "resets_at": "1717800000"}
        }
        """.data(using: .utf8)!
        let result = UsageNormalizer.claude(json)
        XCTAssertNotNil(result)
        XCTAssertEqual(result?.h5.reset, 1717600000)
        XCTAssertEqual(result?.d7.reset, 1717800000)
    }

    func testClaudeMissingFiveHourReturnsNil() {
        let json = """
        {
            "seven_day": {"utilization": 50, "resets_at": 1000}
        }
        """.data(using: .utf8)!
        XCTAssertNil(UsageNormalizer.claude(json))
    }

    func testClaudeMissingSevenDayReturnsNil() {
        let json = """
        {
            "five_hour": {"utilization": 50, "resets_at": 1000}
        }
        """.data(using: .utf8)!
        XCTAssertNil(UsageNormalizer.claude(json))
    }

    func testClaudeGarbageReturnsNil() {
        XCTAssertNil(UsageNormalizer.claude(Data("not json".utf8)))
        XCTAssertNil(UsageNormalizer.claude(Data()))
        XCTAssertNil(UsageNormalizer.claude(Data("[]".utf8)))
    }

    func testClaudeResetZeroWhenAbsent() {
        let json = """
        {
            "five_hour": {"utilization": 10},
            "seven_day": {"utilization": 20}
        }
        """.data(using: .utf8)!
        let result = UsageNormalizer.claude(json)
        XCTAssertNotNil(result)
        XCTAssertEqual(result?.h5.reset, 0)
        XCTAssertEqual(result?.d7.reset, 0)
    }

    // MARK: - Codex

    func testCodexHappyPath() {
        let json = """
        {
            "rate_limit": {
                "primary_window": {"used_percent": 1.0, "reset_at": 1717590000},
                "secondary_window": {"used_percent": 29.0, "reset_at": 1717800000}
            }
        }
        """.data(using: .utf8)!
        let result = UsageNormalizer.codex(json)
        XCTAssertNotNil(result)
        XCTAssertEqual(result?.h5.pct, 1)
        XCTAssertEqual(result?.d7.pct, 29)
        XCTAssertEqual(result?.h5.reset, 1717590000)
        XCTAssertEqual(result?.d7.reset, 1717800000)
    }

    func testCodexIntegerPercent() {
        let json = """
        {
            "rate_limit": {
                "primary_window": {"used_percent": 75, "reset_at": 1000},
                "secondary_window": {"used_percent": 42, "reset_at": 2000}
            }
        }
        """.data(using: .utf8)!
        let result = UsageNormalizer.codex(json)
        XCTAssertNotNil(result)
        XCTAssertEqual(result?.h5.pct, 75)
        XCTAssertEqual(result?.d7.pct, 42)
    }

    func testCodexClamping() {
        let json = """
        {
            "rate_limit": {
                "primary_window": {"used_percent": 200.0, "reset_at": 0},
                "secondary_window": {"used_percent": -10.0, "reset_at": 0}
            }
        }
        """.data(using: .utf8)!
        let result = UsageNormalizer.codex(json)
        XCTAssertNotNil(result)
        XCTAssertEqual(result?.h5.pct, 100)
        XCTAssertEqual(result?.d7.pct, 0)
    }

    func testCodexMissingRateLimitReturnsNil() {
        let json = """
        {"something_else": {}}
        """.data(using: .utf8)!
        XCTAssertNil(UsageNormalizer.codex(json))
    }

    // Plans without a 5h window (e.g. Plus, observed 2026-07-16) send secondary_window:null and a
    // primary_window that is the 7d window; the normalizer routes by limit_window_seconds.
    func testCodexSingleWindowPlans() {
        let cases: [(name: String, json: String, h5: Int?, d7: Int?, d7Reset: Int)] = [
            (
                "plus: null secondary, primary is 7d",
                """
                {
                    "rate_limit": {
                        "allowed": true,
                        "primary_window": {"used_percent": 1, "limit_window_seconds": 604800,
                                           "reset_after_seconds": 517317, "reset_at": 1784689875},
                        "secondary_window": null
                    }
                }
                """,
                nil, 1, 1784689875
            ),
            (
                "missing secondary, primary is 5h",
                """
                {
                    "rate_limit": {
                        "primary_window": {"used_percent": 10, "limit_window_seconds": 18000, "reset_at": 1000}
                    }
                }
                """,
                10, nil, 0
            ),
            (
                "only secondary present, no window seconds (positional fallback => 7d)",
                """
                {
                    "rate_limit": {
                        "secondary_window": {"used_percent": 10, "reset_at": 1000}
                    }
                }
                """,
                nil, 10, 1000
            ),
        ]
        for c in cases {
            let result = UsageNormalizer.codex(c.json.data(using: .utf8)!)
            XCTAssertNotNil(result, c.name)
            XCTAssertEqual(result?.h5.pct, c.h5, c.name)
            XCTAssertEqual(result?.d7.pct, c.d7, c.name)
            XCTAssertEqual(result?.d7.reset, c.d7Reset, c.name)
        }
    }

    func testCodexNoWindowsReturnsNil() {
        let json = """
        {"rate_limit": {"allowed": true, "primary_window": null, "secondary_window": null}}
        """.data(using: .utf8)!
        XCTAssertNil(UsageNormalizer.codex(json))
    }

    func testCodexGarbageReturnsNil() {
        XCTAssertNil(UsageNormalizer.codex(Data("garbage".utf8)))
        XCTAssertNil(UsageNormalizer.codex(Data()))
    }

    func testCodexNilPctWhenMissing() {
        let json = """
        {
            "rate_limit": {
                "primary_window": {"reset_at": 1000},
                "secondary_window": {"reset_at": 2000}
            }
        }
        """.data(using: .utf8)!
        let result = UsageNormalizer.codex(json)
        XCTAssertNotNil(result)
        XCTAssertNil(result?.h5.pct)
        XCTAssertNil(result?.d7.pct)
    }

    func testCodexResetAsISO() {
        let json = """
        {
            "rate_limit": {
                "primary_window": {"used_percent": 5, "reset_at": "2026-06-11T12:00:00Z"},
                "secondary_window": {"used_percent": 10, "reset_at": "2026-06-15T00:00:00Z"}
            }
        }
        """.data(using: .utf8)!
        let result = UsageNormalizer.codex(json)
        XCTAssertNotNil(result)
        XCTAssertGreaterThan(result!.h5.reset, 0)
        XCTAssertGreaterThan(result!.d7.reset, 0)
    }

    func testCodexExtraKeysIgnored() {
        let json = """
        {
            "rate_limit": {
                "primary_window": {"used_percent": 50, "reset_at": 1000, "extra_key": "ignored"},
                "secondary_window": {"used_percent": 60, "reset_at": 2000, "another": 42}
            },
            "unknown_top_level": true
        }
        """.data(using: .utf8)!
        let result = UsageNormalizer.codex(json)
        XCTAssertNotNil(result)
        XCTAssertEqual(result?.h5.pct, 50)
        XCTAssertEqual(result?.d7.pct, 60)
    }
}
