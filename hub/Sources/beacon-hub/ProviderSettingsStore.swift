import Foundation
import BeaconHubKit

// UserDefaults conformance for the pure ProviderSettings logic (design 2026-07-19). The kit stays
// Foundation-only + unit-testable with an in-memory store; the persistence binding lives here.
extension UserDefaults: KeyValueStore {
    public func settingsBool(forKey key: String) -> Bool { bool(forKey: key) }
    public func settingsHasValue(forKey key: String) -> Bool { object(forKey: key) != nil }
    public func settingsSet(_ value: Bool, forKey key: String) { set(value, forKey: key) }
}
