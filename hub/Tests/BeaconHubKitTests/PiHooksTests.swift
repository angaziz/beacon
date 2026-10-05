import XCTest
@testable import BeaconHubKit

final class PiHooksTests: XCTestCase {

    func testExtensionSourceCarriesSessionWireContract() {
        let source = PiHooks.extensionSource
        XCTAssertTrue(source.hasPrefix("// beacon-pi v3"), "marker must be on line 1")
        XCTAssertTrue(source.contains("http://127.0.0.1:8765/pi/hook"))
        XCTAssertTrue(source.contains("getSessionFile"), "session file gives a stable resumed-session id")
        XCTAssertTrue(source.contains("TERM_PROGRAM"), "captures host app for tap-to-open")
        XCTAssertTrue(source.contains("WARP_FOCUS_URL"), "captures Warp focus handle")
        XCTAssertTrue(source.contains("pi.on(\"agent_settled\""), "settled, not agent_end, means idle")
    }

    func testExtensionGatesStandalonePromptsAndStepsDownForPps() {
        let source = PiHooks.extensionSource
        for required in ["pi.on(\"tool_call\"", "/pi/permission", "ctx.ui.select", "ppsActive", "realpath", "CIRCUIT_MS", "commit(payload.id, false)", "block: true"] {
            XCTAssertTrue(source.contains(required), "pi gate missing \(required)")
        }
    }

    func testAllHandlersGuardOnHasUI() {
        let source = PiHooks.extensionSource
        guard let begin = source.range(of: "const beginSession"),
              let sessionStart = source.range(of: "pi.on(\"session_start\"") else {
            return XCTFail("missing session-start binding")
        }
        XCTAssertTrue(source[begin.lowerBound..<sessionStart.lowerBound].contains("if (!ctx.hasUI) return;"))

        for event in ["agent_start", "agent_settled", "session_shutdown"] {
            guard let range = source.range(of: "pi.on(\"\(event)\"") else {
                return XCTFail("missing handler for \(event)")
            }
            let body = source[range.upperBound...].prefix(100)
            XCTAssertTrue(body.contains("if (!ctx.hasUI) return;"), "\(event) must guard on ctx.hasUI")
        }
    }

    func testPpsAuthorizerChainMergePreservesPolicyBytes() {
        let source = """
        {
          "permission":{"bash":{"*":"allow","rm *":"ask"}},
          "authorizerChain":["first", "second"]
        }
        """
        guard let merged = PiHooks.mergePpsAuthorizerChain(source) else { return XCTFail("valid config must merge") }
        XCTAssertEqual(merged, source.replacingOccurrences(of: "]", with: ", \"beacon\"]", options: [], range: source.range(of: "]")))
        XCTAssertTrue(PiHooks.ppsAuthorizerChainContainsBeacon(merged))
        XCTAssertEqual(PiHooks.mergePpsAuthorizerChain(merged), merged, "merge is idempotent")
    }

    func testPpsAuthorizerChainMergeAddsMissingKeyWithoutReserializing() {
        let source = "{\n  \"permission\": {\"*\": \"ask\", \"rm *\": \"deny\"}\n}\n"
        let expected = "{\n  \"authorizerChain\":[\"beacon\"],\n  \"permission\": {\"*\": \"ask\", \"rm *\": \"deny\"}\n}\n"
        XCTAssertEqual(PiHooks.mergePpsAuthorizerChain(source), expected)
    }

    // Regression (codex review, PR #153): `{}` is a valid all-defaults config; the merge must not
    // emit a trailing comma (which would overwrite the user's valid config with invalid JSON).
    func testPpsAuthorizerChainMergeHandlesEmptyConfig() {
        let cases: [(name: String, source: String, expected: String)] = [
            ("bare", "{}", "{\"authorizerChain\":[\"beacon\"]}"),
            ("whitespace", "{\n}\n", "{\"authorizerChain\":[\"beacon\"]\n}\n"),
        ]
        for entry in cases {
            guard let merged = PiHooks.mergePpsAuthorizerChain(entry.source) else { return XCTFail("\(entry.name): must merge") }
            XCTAssertEqual(merged, entry.expected, entry.name)
            XCTAssertNotNil(try? JSONSerialization.jsonObject(with: Data(merged.utf8)), "\(entry.name): merged output must be valid JSON")
        }
    }

    func testPpsAuthorizerRejectsInvalidConfigs() {
        let cases: [(name: String, source: String)] = [
            ("not json", "not json"),
            ("top-level array", "[\"beacon\"]"),
            ("non-string chain entry", "{\"authorizerChain\":[\"beacon\", 1]}"),
            ("chain not an array", "{\"authorizerChain\":\"beacon\"}"),
        ]
        for entry in cases {
            XCTAssertNil(PiHooks.mergePpsAuthorizerChain(entry.source), entry.name)
            XCTAssertFalse(PiHooks.ppsAuthorizerChainContainsBeacon(entry.source), entry.name)
        }
    }

    // Regression (#154): pps 39 added top-level keys; an allowlist made such configs unmergeable, so
    // Settings showed "Set up" forever. Unknown keys must be preserved byte-for-byte.
    func testPpsAuthorizerChainMergeToleratesUnknownKeys() {
        let cases: [(name: String, source: String, expected: String)] = [
            ("pps 39 keys with chain",
             "{\"forwardingTimeoutMs\":30000,\"promptNotifications\":true,\"authorizerChain\":[\"first\"]}",
             "{\"forwardingTimeoutMs\":30000,\"promptNotifications\":true,\"authorizerChain\":[\"first\", \"beacon\"]}"),
            ("pps 39 keys without chain",
             "{\"permissionDialogKeys\":{\"allow\":\"y\"},\"promptMaxRows\":8}",
             "{\"authorizerChain\":[\"beacon\"],\"permissionDialogKeys\":{\"allow\":\"y\"},\"promptMaxRows\":8}"),
            ("future key", "{\"someFutureKey\":[1,2]}", "{\"authorizerChain\":[\"beacon\"],\"someFutureKey\":[1,2]}"),
        ]
        for entry in cases {
            guard let merged = PiHooks.mergePpsAuthorizerChain(entry.source) else { return XCTFail("\(entry.name): must merge") }
            XCTAssertEqual(merged, entry.expected, entry.name)
            XCTAssertTrue(PiHooks.ppsAuthorizerChainContainsBeacon(merged), entry.name)
        }
    }

    func testIsCurrent() {
        let source = PiHooks.extensionSource
        let cases: [(name: String, content: String, expected: Bool)] = [
            ("exact", source, true),
            ("trailing newline", source + "\n", true),
            ("leading/trailing whitespace", "\n  " + source + "  \n", true),
            ("empty", "", false),
            ("truncated", String(source.dropLast(100)), false),
            ("one char flipped", flipOneChar(source), false),
            ("version variant", source.replacingOccurrences(of: "beacon-pi v3", with: "beacon-pi v30"), false),
            ("unrelated content", "export default function () {}", false)
        ]
        for testCase in cases {
            XCTAssertEqual(PiHooks.isCurrent(testCase.content), testCase.expected, "isCurrent(\(testCase.name))")
        }
    }

    private func flipOneChar(_ string: String) -> String {
        var characters = Array(string)
        guard let index = characters.lastIndex(where: { !$0.isWhitespace }) else { return string + "x" }
        characters[index] = characters[index] == "x" ? "y" : "x"
        return String(characters)
    }
}
