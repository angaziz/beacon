import XCTest
@testable import BeaconHubKit

// In-memory KeyValueStore so ProviderSettings is tested without UserDefaults.
private final class MemStore: KeyValueStore {
    private var values: [String: Bool] = [:]
    func settingsBool(forKey key: String) -> Bool { values[key] ?? false }
    func settingsHasValue(forKey key: String) -> Bool { values[key] != nil }
    func settingsSet(_ value: Bool, forKey key: String) { values[key] = value }
}

final class ProviderSettingsTests: XCTestCase {
    func testDefaultsAllOnWhenUnset() {
        let s = ProviderSettings(store: MemStore())
        let e = s.enabled(for: "claude")
        XCTAssertTrue(e.usage)
        XCTAssertTrue(e.buddy)
    }

    func testSetPersistsPerToggleAndProvider() {
        let store = MemStore()
        let s = ProviderSettings(store: store)
        s.setUsage(false, for: "claude")
        s.setBuddy(false, for: "codex")

        let claude = s.enabled(for: "claude")
        XCTAssertFalse(claude.usage)
        XCTAssertTrue(claude.buddy, "buddy untouched for claude => still default on")

        let codex = s.enabled(for: "codex")
        XCTAssertTrue(codex.usage, "usage untouched for codex => still default on")
        XCTAssertFalse(codex.buddy)
    }

    func testExplicitTrueReadsBack() {
        let s = ProviderSettings(store: MemStore())
        s.setUsage(false, for: "claude")
        XCTAssertFalse(s.enabled(for: "claude").usage)
        s.setUsage(true, for: "claude")
        XCTAssertTrue(s.enabled(for: "claude").usage)   // explicit true, not just the absent default
    }
}
