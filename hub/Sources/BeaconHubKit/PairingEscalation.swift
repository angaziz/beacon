import Foundation

// Pure bounded-retry decision for a first-time BLE pair, split out of BeaconCentral so the load-bearing
// escalation rule is table-testable without CoreBluetooth. No timers: the caller supplies `now` from a
// MONOTONIC clock. Escalates only on a FIRST-time pair (hadConnection == false); a previously-good
// device that drops keeps auto-reconnecting (hadConnection == true never escalates).
public struct PairingEscalation {
    private let maxAttempts: Int
    private let deadline: TimeInterval
    private var attempts = 0
    private var firstAt: TimeInterval?

    public init(maxAttempts: Int, deadline: TimeInterval) {
        self.maxAttempts = maxAttempts
        self.deadline = deadline
    }

    // Stamp the first-attempt time once per attempt sequence so the deadline clock starts at the first
    // scan, not on every rescan.
    public mutating func recordAttemptStart(now: TimeInterval) {
        if firstAt == nil { firstAt = now }
    }

    // Returns true (=> escalate, stop the silent rescan loop) only on a first-time pair that has either
    // burned the attempt budget or exceeded the deadline.
    public mutating func recordFailure(now: TimeInterval, hadConnection: Bool) -> Bool {
        attempts += 1
        guard !hadConnection else { return false }
        let elapsed = now - (firstAt ?? now)
        return attempts >= maxAttempts || elapsed >= deadline
    }

    public mutating func reset() {
        attempts = 0
        firstAt = nil
    }
}
