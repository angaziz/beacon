import Foundation

// Persisted BLE kill-switch preference (#146). The key is absent until the user first flips the toggle
// and MUST resolve to enabled -- UserDefaults.bool(forKey:) folds an absent key into false, which would
// silently ship the link off. Centralizing the rule keeps the launch read (AppDelegate) and the toggle
// seed (menubar UI) in agreement.
public enum LinkPreference {
    public static let key = "BeaconLinkEnabled"

    public static func isEnabled(stored: Any?) -> Bool {
        (stored as? Bool) ?? true
    }
}
