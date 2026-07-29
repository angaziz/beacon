import XCTest
@testable import BeaconHubKit

final class HooksDetectionTests: XCTestCase {

    private let shim = "/Users/x/.beacon/beacon-statusline"
    private let hookShim = "/Users/x/.beacon/beacon-claude-hook"

    // A fully-installed settings dict: PermissionRequest with the beacon hook shim + statusLine shim.
    private func installed() -> [String: Any] {
        [
            "hooks": [
                "PermissionRequest": [
                    ["matcher": "*", "hooks": [
                        ["type": "command", "command": hookShim, "timeout": 600],
                    ]],
                ],
            ],
            "statusLine": ["type": "command", "command": shim],
        ]
    }

    func testIsInstalled() {
        struct Case { let name: String; let settings: [String: Any]; let want: Bool }

        // PermissionRequest present but statusLine references the bundled (space-containing) path, not
        // the no-space install path => not installed (detection/install must agree on the same path).
        var wrongShim = installed()
        wrongShim["statusLine"] = ["type": "command", "command": "/Apps/Beacon Hub.app/Contents/Resources/beacon-statusline"]

        // Only the OPTIONAL SessionStart hook is wired; the essential PermissionRequest is absent.
        let noPermissionRequest: [String: Any] = [
            "hooks": [
                "SessionStart": [
                    ["matcher": "startup", "hooks": [["type": "command", "command": hookShim]]],
                ],
            ],
            "statusLine": ["type": "command", "command": shim],
        ]

        // PermissionRequest beacon hook present but no statusLine at all.
        var noStatusline = installed()
        noStatusline.removeValue(forKey: "statusLine")

        // Pre-shim install: the http hook still reaches the hub, but prints ECONNREFUSED in CC whenever
        // the hub is down => report NOT installed so Settings offers the migrating reinstall.
        var legacyHTTP = installed()
        legacyHTTP["hooks"] = [
            "PermissionRequest": [
                ["matcher": "*", "hooks": [["type": "http", "url": HooksDetection.beaconHookURL, "timeout": 600]]],
            ],
        ]

        // Another tool's hook whose command merely ENDS in our shim name is not ours.
        var lookalike = installed()
        lookalike["hooks"] = [
            "PermissionRequest": [
                ["matcher": "*", "hooks": [["type": "command", "command": "/opt/other/beacon-claude-hook"]]],
            ],
        ]

        // Beacon's wrapper sitting alongside a third-party PermissionRequest wrapper (the common case).
        var coexisting = installed()
        coexisting["hooks"] = [
            "PermissionRequest": [
                ["matcher": "*", "hooks": [["type": "command", "command": "/opt/other/bridge"]]],
                ["matcher": "*", "hooks": [["type": "command", "command": hookShim, "timeout": 600]]],
            ],
        ]

        let cases: [Case] = [
            Case(name: "empty (proxy for missing file)", settings: [:], want: false),
            Case(name: "no PermissionRequest", settings: noPermissionRequest, want: false),
            Case(name: "PermissionRequest but no statusline", settings: noStatusline, want: false),
            Case(name: "statusline without shim path", settings: wrongShim, want: false),
            Case(name: "legacy http hook", settings: legacyHTTP, want: false),
            Case(name: "lookalike command path", settings: lookalike, want: false),
            Case(name: "coexisting with a third-party hook", settings: coexisting, want: true),
            Case(name: "fully installed", settings: installed(), want: true),
        ]

        for c in cases {
            XCTAssertEqual(
                HooksDetection.isInstalled(settings: c.settings, shimPath: shim, hookShimPath: hookShim),
                c.want, "case: \(c.name)")
        }
    }
}
