import XCTest
@testable import BeaconHubKit

final class HooksDetectionTests: XCTestCase {

    private let shim = "/Users/x/.beacon/beacon-statusline"

    // A fully-installed settings dict: PermissionRequest with the beacon http hook + statusLine shim.
    private func installed() -> [String: Any] {
        [
            "hooks": [
                "PermissionRequest": [
                    ["matcher": "*", "hooks": [
                        ["type": "http", "url": HooksDetection.beaconHookURL, "timeout": 190],
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
                    ["matcher": "startup", "hooks": [["type": "http", "url": HooksDetection.beaconHookURL]]],
                ],
            ],
            "statusLine": ["type": "command", "command": shim],
        ]

        // PermissionRequest beacon hook present but no statusLine at all.
        var noStatusline = installed()
        noStatusline.removeValue(forKey: "statusLine")

        let cases: [Case] = [
            Case(name: "empty (proxy for missing file)", settings: [:], want: false),
            Case(name: "no PermissionRequest", settings: noPermissionRequest, want: false),
            Case(name: "PermissionRequest but no statusline", settings: noStatusline, want: false),
            Case(name: "statusline without shim path", settings: wrongShim, want: false),
            Case(name: "fully installed", settings: installed(), want: true),
        ]

        for c in cases {
            XCTAssertEqual(
                HooksDetection.isInstalled(settings: c.settings, shimPath: shim), c.want,
                "case: \(c.name)")
        }
    }
}
