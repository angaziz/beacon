import XCTest
@testable import BeaconHubKit

final class UsageHistoryTests: XCTestCase {
    func testSampleRoundTripsJSON() throws {
        let s = UsageSample(ts: 1_780_000_000, provider: .claude, h5: 42, d7: 7)
        let data = try JSONEncoder().encode(s)
        let back = try JSONDecoder().decode(UsageSample.self, from: data)
        XCTAssertEqual(s, back)
    }

    func testSeriesSplitsOnGap() {
        var samples: [UsageSample] = []
        for i in 0..<3 { samples.append(UsageSample(ts: 1000 + i*60, provider: .claude, h5: 10 + i, d7: 1)) }
        for i in 0..<3 { samples.append(UsageSample(ts: 1000 + 20*60 + i*60, provider: .claude, h5: 20 + i, d7: 2)) }
        let series = ChartSeries.build(samples: samples, provider: .claude, metric: .h5,
                                       range: 0...(1000 + 30*60), breakSeconds: 300)
        XCTAssertEqual(series.segments.count, 2)
        XCTAssertEqual(series.segments[0].count, 3)
        XCTAssertEqual(series.segments[1].count, 3)
    }

    func testSeriesFiltersToRangeAndProvider() {
        let samples = [
            UsageSample(ts: 100, provider: .claude, h5: 5, d7: 1),
            UsageSample(ts: 200, provider: .codex, h5: 9, d7: 2),
            UsageSample(ts: 300, provider: .claude, h5: 7, d7: 1),
        ]
        let series = ChartSeries.build(samples: samples, provider: .claude, metric: .h5, range: 150...400, breakSeconds: 300)
        XCTAssertEqual(series.segments.flatMap { $0 }.map { $0.pct }, [7])
    }
}
