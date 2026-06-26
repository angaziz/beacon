import XCTest
@testable import beacon_hub

final class SessionFocusTests: XCTestCase {

    // --- SessionFocus.plan table tests ---

    func testPlanFocusURL() {
        // Tier 1: focus_url present => .url regardless of other fields.
        let t = FocusTarget(hostApp: "WarpTerminal", focusURL: "warp://focus/abc", bundleId: "dev.warp.Warp", cwd: "/tmp")
        XCTAssertEqual(SessionFocus.plan(t), .url("warp://focus/abc"))
    }

    func testPlanEditorReuseCursor() {
        // Tier 2: hostApp "Cursor" (no focus_url) => .editorReuse.
        let t = FocusTarget(hostApp: "Cursor", focusURL: nil, bundleId: "com.todesktop.230313mzl4w4u92", cwd: "/Users/me/proj")
        XCTAssertEqual(SessionFocus.plan(t), .editorReuse(cwd: "/Users/me/proj"))
    }

    func testPlanEditorReuseVSCode() {
        // Tier 2: hostApp "vscode" => .editorReuse.
        let t = FocusTarget(hostApp: "vscode", focusURL: nil, bundleId: "com.microsoft.VSCode", cwd: "/src")
        XCTAssertEqual(SessionFocus.plan(t), .editorReuse(cwd: "/src"))
    }

    func testPlanOpenBundle() {
        // Tier 3a: bundleId present, not an editor, no focus_url.
        let t = FocusTarget(hostApp: nil, focusURL: nil, bundleId: "dev.warp.Warp", cwd: "/tmp")
        XCTAssertEqual(SessionFocus.plan(t), .openBundle("dev.warp.Warp"))
    }

    func testPlanOpenApp() {
        // Tier 3b: hostApp only (no bundleId, not an editor).
        let t = FocusTarget(hostApp: "ghostty", focusURL: nil, bundleId: nil, cwd: "/tmp")
        XCTAssertEqual(SessionFocus.plan(t), .openApp("Ghostty"))
    }

    func testPlanNone() {
        // No context at all.
        let t = FocusTarget(hostApp: nil, focusURL: nil, bundleId: nil, cwd: "")
        XCTAssertEqual(SessionFocus.plan(t), .none)
    }

    func testPlanFocusURLTakesPrecedenceOverEditor() {
        // Even if hostApp is Cursor, a focus_url wins (Tier 1 > Tier 2).
        let t = FocusTarget(hostApp: "Cursor", focusURL: "warp://focus/xyz", bundleId: nil, cwd: "/src")
        XCTAssertEqual(SessionFocus.plan(t), .url("warp://focus/xyz"))
    }

    func testPlanEmptyFocusURLFallsThrough() {
        // Empty focus_url is treated as absent; falls to next tier.
        let t = FocusTarget(hostApp: "ghostty", focusURL: "", bundleId: "com.mitchellh.ghostty", cwd: "/tmp")
        XCTAssertEqual(SessionFocus.plan(t), .openBundle("com.mitchellh.ghostty"))
    }

    // --- SessionFocus.appName mapping tests ---

    func testAppNameMappings() {
        let cases: [(input: String, expected: String)] = [
            ("WarpTerminal",   "Warp"),
            ("ghostty",        "Ghostty"),
            ("Apple_Terminal", "Terminal"),
            ("iTerm.app",      "iTerm"),
            ("MyTerm.app",     "MyTerm"),   // strip .app suffix
            ("xterm",          "xterm"),    // pass-through
        ]
        for c in cases {
            XCTAssertEqual(SessionFocus.appName(for: c.input), c.expected, "input: \(c.input)")
        }
    }
}
