import XCTest
@testable import BeaconHubKit

// Pure omp-extension logic: the managed extension source carries the wire contract, and isCurrent is
// exact-content equality (not a marker substring) so stale/truncated/edited files read as NOT current.
final class OmpHooksTests: XCTestCase {

    func testExtensionSourceCarriesWireContract() {
        let src = OmpHooks.extensionSource
        XCTAssertTrue(src.hasPrefix("// beacon-omp v4"), "marker must be on line 1")
        XCTAssertTrue(src.contains("http://127.0.0.1:8765/omp/hook"))
        XCTAssertTrue(src.contains("TERM_PROGRAM"), "captures host app for tap-to-open")
        XCTAssertTrue(src.contains("WARP_FOCUS_URL"), "captures Warp focus handle")
        XCTAssertTrue(src.contains("lifecycle(\"Notification\")"), "approval prompt => question state")
        XCTAssertTrue(src.contains("lifecycle(\"ApprovalResolved\")"), "answered prompt clears question")
    }

    // v4 mirrors approvals instead of gating them: omp settles approval before extensions see
    // `tool_call`, so a gate there would double-prompt (Mac then device) or fire under `yolo`, which
    // told omp not to ask at all. Nothing in the source may block a tool or send a PermissionRequest.
    func testExtensionNeverGatesToolCalls() {
        let src = OmpHooks.extensionSource
        // Code forms, not bare words: the source comments legitimately explain why the v3 tool_call
        // gate and its PermissionRequest POST are gone, so a substring match would flag its own rationale.
        for forbidden in ["pi.on(\"tool_call\"", "hook_event_name: \"PermissionRequest\"",
                          "GATED_TOOLS", "block: true"] {
            XCTAssertFalse(src.contains(forbidden), "v4 must not emit \(forbidden)")
        }
    }

    // Both approval mirrors re-check ctx.hasUI: a task subagent runs inside an already-bound
    // interactive session, so the cached sessionId alone cannot exclude it.
    func testApprovalMirrorsGuardOnHasUI() {
        let src = OmpHooks.extensionSource
        let mirrors = ["tool_approval_requested", "tool_approval_resolved"]
        for event in mirrors {
            guard let r = src.range(of: "pi.on(\"\(event)\"") else {
                return XCTFail("missing handler for \(event)")
            }
            let body = src[r.upperBound...].prefix(120)
            XCTAssertTrue(body.contains("if (!ctx.hasUI) return;"), "\(event) must guard on ctx.hasUI")
        }
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
            ("v40 marker variant", src.replacingOccurrences(of: "beacon-omp v4", with: "beacon-omp v40"), false),
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
