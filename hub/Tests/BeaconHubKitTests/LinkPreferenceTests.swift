import XCTest
import BeaconHubKit

// #146: the BLE kill-switch key is absent until the user first flips the toggle; absent MUST mean
// enabled (a literal bool(forKey:) read would default the link to off).
final class LinkPreferenceTests: XCTestCase {

    func testIsEnabled() {
        let cases: [(name: String, stored: Any?, want: Bool)] = [
            ("absent key defaults to enabled", nil, true),
            ("stored true stays enabled", true, true),
            ("stored false disables", false, false),
            ("non-bool garbage falls back to enabled", "off", true),
        ]
        for c in cases {
            XCTAssertEqual(LinkPreference.isEnabled(stored: c.stored), c.want, c.name)
        }
    }
}
