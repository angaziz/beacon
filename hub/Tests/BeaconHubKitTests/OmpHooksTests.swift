import XCTest
@testable import BeaconHubKit

// Pure omp-extension logic: the managed extension source carries the wire contract, and isCurrent is
// exact-content equality (not a marker substring) so stale/truncated/edited files read as NOT current.
final class OmpHooksTests: XCTestCase {

    func testExtensionSourceCarriesWireContract() {
        let src = OmpHooks.extensionSource
        XCTAssertTrue(src.hasPrefix("// beacon-omp v3"), "marker must be on line 1")
        XCTAssertTrue(src.contains("http://127.0.0.1:8765/omp/hook"))
        XCTAssertTrue(src.contains("\"PermissionRequest\""))
        XCTAssertTrue(src.contains("GATED_TOOLS = new Set([\"bash\"])"))
        XCTAssertTrue(src.contains("TERM_PROGRAM"), "captures host app for tap-to-open")
        XCTAssertTrue(src.contains("WARP_FOCUS_URL"), "captures Warp focus handle")
        XCTAssertTrue(src.contains("if (!ctx.hasUI || !sessionId"),
                      "tool_call must gate on ctx.hasUI, not just the cached session id (subagents share it)")
    }

    func testIsCurrent() {
        let src = OmpHooks.extensionSource
        let cases: [(name: String, content: String, expected: Bool)] = [
            ("exact", src, true),
            ("trailing newline", src + "\n", true),
            ("leading/trailing whitespace", "\n  " + src + "  \n", true),
            ("empty", "", false),
            ("truncated", String(src.dropLast(100)), false),
            ("one char flipped", flipOneChar(src), false),
            ("v30 marker variant", src.replacingOccurrences(of: "beacon-omp v3", with: "beacon-omp v30"), false),
            ("unrelated content", "export default function () {}", false),
        ]
        for c in cases {
            XCTAssertEqual(OmpHooks.isCurrent(c.content), c.expected, "isCurrent(\(c.name))")
        }
    }

    // Flip the last non-whitespace character so a trimming compare still sees a difference.
    private func flipOneChar(_ s: String) -> String {
        var chars = Array(s)
        guard let idx = chars.lastIndex(where: { !$0.isWhitespace }) else { return s + "x" }
        chars[idx] = chars[idx] == "x" ? "y" : "x"
        return String(chars)
    }
}
