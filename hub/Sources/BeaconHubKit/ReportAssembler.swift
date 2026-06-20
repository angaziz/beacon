import Foundation

// Per-connection reassembler for the device -> hub cmd:"report" chunks (issue #105). Mirrors the
// firmware config_accum_t: part 0 (re)starts; later parts must be contiguous and share `parts`. Any
// gap / duplicate-past-0 / parts-mismatch discards the partial and returns .dropped (fail-closed). The
// owner resets it on the disconnect edge. Pure + host-testable; non-report commands return .pending
// (ignored, no effect on an in-progress accumulation).
public enum ReportResult: Equatable {
    case pending
    case dropped
    case assembled([TickerRow])
}

public struct ReportAssembler {
    private var rev: UInt32 = 0
    private var parts = 0
    private var nextPart = 0
    private var rows = [TickerRow]()
    private var active = false

    public init() {}

    public mutating func reset() {
        rev = 0; parts = 0; nextPart = 0; rows.removeAll(); active = false
    }

    public mutating func feed(_ cmd: DeviceCommand) -> ReportResult {
        guard case let .report(_, rev, part, parts, chunkRows) = cmd else { return .pending }

        if part == 0 {                                  // part 0 always (re)starts a fresh accumulation
            active = true; self.rev = rev; self.parts = parts; nextPart = 0; rows.removeAll()
        } else if !active || rev != self.rev || parts != self.parts || part != nextPart {
            reset()                                     // out-of-order / rev or parts mismatch / no active run
            return .dropped
        }

        rows.append(contentsOf: chunkRows)
        nextPart = part + 1
        guard nextPart >= self.parts else { return .pending }

        let assembled = rows
        reset()
        return .assembled(assembled)
    }
}
