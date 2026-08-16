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

    func testPpsAuthorizerRejectsInvalidConfigs() {
        XCTAssertNil(PiHooks.mergePpsAuthorizerChain("not json"))
        XCTAssertNil(PiHooks.mergePpsAuthorizerChain("{\"unknown\":true}"))
        XCTAssertNil(PiHooks.mergePpsAuthorizerChain("{\"authorizerChain\":[\"beacon\", 1]}"))
        XCTAssertFalse(PiHooks.ppsAuthorizerChainContainsBeacon("{\"unknown\":true,\"authorizerChain\":[\"beacon\"]}"))
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
