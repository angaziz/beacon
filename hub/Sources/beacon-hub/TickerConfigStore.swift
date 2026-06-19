import Foundation
import BeaconHubKit

// UserDefaults-backed home for the desired ticker list + monotonic rev (issue #92, B3). Thin wrapper
// around the pure TickerConfigState: the rev/row logic lives there (host-tested), this only handles JSON
// persistence. `defaults` is injectable so a suite-name store can exercise save+reload in tests.
final class TickerConfigStore {
    private static let key = "BeaconTickerConfig"

    private let defaults: UserDefaults
    private(set) var current: TickerConfigState

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        // Absent or corrupt (older/garbage payload) => start clean; a fresh push reseeds rev from 0.
        if let data = defaults.data(forKey: Self.key),
           let state = try? JSONDecoder().decode(TickerConfigState.self, from: data) {
            current = state
        } else {
            current = TickerConfigState()
        }
    }

    func save(rows: [TickerRow]) {
        current = current.updating(rows: rows)
        do {
            let data = try JSONEncoder().encode(current)
            defaults.set(data, forKey: Self.key)
        } catch {
            FileHandle.standardError.write(Data("[beacon-hub] ticker config encode failed (not persisted): \(error.localizedDescription)\n".utf8))
        }
    }
}
