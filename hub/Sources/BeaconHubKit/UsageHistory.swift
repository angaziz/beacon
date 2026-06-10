import Foundation

public enum UsageProviderKind: String, Codable, Equatable { case claude, codex }

// One ring-buffer sample (~1/min). Compact keys keep the JSONL file small (low single-digit MB over 14d).
public struct UsageSample: Codable, Equatable {
    public let ts: Int                 // epoch seconds
    public let provider: UsageProviderKind
    public let h5: Int?                // 5h-window pct
    public let d7: Int?                // 7d-window pct
    public init(ts: Int, provider: UsageProviderKind, h5: Int?, d7: Int?) {
        self.ts = ts; self.provider = provider; self.h5 = h5; self.d7 = d7
    }
    enum CodingKeys: String, CodingKey { case ts = "t", provider = "p", h5, d7 }
}

public enum ChartMetric { case h5, d7 }

public struct ChartPoint: Equatable { public let ts: Int; public let pct: Int }

// Gap-aware series: a list of contiguous segments. The chart draws each segment as its own line so a gap
// (Hub down / Mac asleep) renders as a break, never an interpolated diagonal.
public struct ChartSeries: Equatable {
    public let segments: [[ChartPoint]]

    public static func build(samples: [UsageSample], provider: UsageProviderKind, metric: ChartMetric,
                             range: ClosedRange<Int>, breakSeconds: Int) -> ChartSeries {
        let points: [ChartPoint] = samples
            .filter { $0.provider == provider && $0.ts >= range.lowerBound && $0.ts <= range.upperBound }
            .sorted { $0.ts < $1.ts }
            .compactMap { s in
                let pct = metric == .h5 ? s.h5 : s.d7
                return pct.map { ChartPoint(ts: s.ts, pct: $0) }
            }
        var segments: [[ChartPoint]] = []
        var current: [ChartPoint] = []
        for p in points {
            if let last = current.last, p.ts - last.ts > breakSeconds {
                segments.append(current); current = []
            }
            current.append(p)
        }
        if !current.isEmpty { segments.append(current) }
        return ChartSeries(segments: segments)
    }
}
