import Foundation

public struct BurnPaceResult: Equatable {
    public enum Style { case warn, safe }
    public let pace: Double?      // pct per hour; nil until enough movement
    public let capEpoch: Int?     // projected time to 100% at current pace; nil if pace nil or non-positive
    public let style: Style?      // warn = caps before window reset; safe = reaches reset first
    public init(pace: Double?, capEpoch: Int?, style: Style?) {
        self.pace = pace; self.capEpoch = capEpoch; self.style = style
    }
}

// Per-provider EMA over recent 5h-window pct samples. Reset on discontinuity (window rollover, big drop,
// long gap) so the estimate never blends across a window boundary. Caller feeds one sample per ~minute.
public struct BurnPaceEstimator {
    private let alpha = 0.4            // EMA smoothing on the per-sample slope
    private let minSamples = 3        // movement samples required before a pace is shown
    private let dropResetPoints = 10
    private let gapResetSeconds = 600

    private var lastPct: Int?
    private var lastNow: Int?
    private var lastReset: Int?
    private var ema: Double?          // pct/hour
    private var movementSamples = 0

    public init() {}

    public mutating func update(pct: Int, reset: Int, now: Int) -> BurnPaceResult {
        defer { lastPct = pct; lastNow = now; lastReset = reset }

        // Discontinuity detection -> clear and restart.
        if let lp = lastPct, let ln = lastNow, let lr = lastReset {
            let dropped = (lp - pct) >= dropResetPoints
            let resetChanged = reset != lr
            let gapped = (now - ln) > gapResetSeconds
            if dropped || resetChanged || gapped {
                ema = nil; movementSamples = 0
                return BurnPaceResult(pace: nil, capEpoch: nil, style: nil)
            }
            let dtHours = Double(now - ln) / 3600.0
            if dtHours > 0 {
                let slope = Double(pct - lp) / dtHours   // pct/hour for this interval
                ema = ema.map { $0 + alpha * (slope - $0) } ?? slope
                if pct != lp { movementSamples += 1 }
            }
        }

        // Withhold the pace until >= 3 movement samples (return nil, NOT the partial ema).
        guard movementSamples >= minSamples, let pace = ema, pace > 0 else {
            return BurnPaceResult(pace: nil, capEpoch: nil, style: nil)
        }
        let remaining = Double(max(0, 100 - pct))
        let hoursToCap = remaining / pace
        let capEpoch = now + Int((hoursToCap * 3600).rounded())
        let style: BurnPaceResult.Style = (reset > 0 && capEpoch >= reset) ? .safe : .warn
        return BurnPaceResult(pace: pace, capEpoch: capEpoch, style: style)
    }
}
