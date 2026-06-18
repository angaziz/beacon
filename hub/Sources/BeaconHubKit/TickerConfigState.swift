import Foundation

// Persisted desired-list snapshot (issue #92, B3). The hub owns the canonical list and a monotonic `rev`
// the device echoes back in config_ack -- bumping on every edit lets the hub ignore stale acks. Pure +
// Codable so the executable's UserDefaults store is a thin wrapper and the rev logic is host-testable.
public struct TickerConfigState: Codable, Equatable {
    public var rows: [TickerRow]
    public var rev: UInt32

    public init(rows: [TickerRow] = [], rev: UInt32 = 0) {
        self.rows = rows
        self.rev = rev
    }

    // New state with the given rows and rev advanced by one. Saturating so a (practically unreachable)
    // UInt32 overflow holds at max rather than wrapping back to a rev the device may have already seen.
    public func updating(rows: [TickerRow]) -> TickerConfigState {
        TickerConfigState(rows: rows, rev: rev == .max ? .max : rev + 1)
    }
}
