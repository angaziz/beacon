import SwiftUI
import Charts
import BeaconHubKit

// Dual-line chart of 5h-window pct (6h/24h) or 7d-window pct (7d) for Claude + Codex. Gaps render as
// line breaks (each ChartSeries segment is its own LineMark series), with a dashed red cap rule at 100%.
// macOS 13 ships Swift Charts, which is the package's deployment target (Package.swift), so no Path fallback.
struct UsageChart: View {
    let claude: ChartSeries
    let codex: ChartSeries
    let range: ClosedRange<Int>
    let axisLabels: [String]   // exactly 3: start / mid / now

    var body: some View {
        VStack(spacing: 2) {
            Chart {
                segmentMarks(claude, color: .blue, name: "Claude")
                segmentMarks(codex, color: .purple, name: "Codex")
                RuleMark(y: .value("Cap", 100))
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [3, 3]))
                    .foregroundStyle(.red.opacity(0.4))
            }
            .chartYScale(domain: 0...100)
            .chartXScale(domain: range.lowerBound...range.upperBound)
            .chartXAxis(.hidden)
            .chartYAxis(.hidden)
            .frame(height: 64)
            HStack {
                ForEach(Array(axisLabels.enumerated()), id: \.offset) { i, label in
                    Text(label).font(.system(size: 9)).foregroundStyle(.tertiary)
                    if i < axisLabels.count - 1 { Spacer() }
                }
            }
        }
    }

    @ChartContentBuilder
    private func segmentMarks(_ series: ChartSeries, color: Color, name: String) -> some ChartContent {
        ForEach(Array(series.segments.enumerated()), id: \.offset) { segIdx, segment in
            ForEach(segment, id: \.ts) { point in
                LineMark(
                    x: .value("t", point.ts),
                    y: .value("pct", point.pct),
                    series: .value("seg", "\(name)-\(segIdx)")   // distinct series => break between segments
                )
                .foregroundStyle(color)
                .lineStyle(StrokeStyle(lineWidth: 1.5))
            }
        }
    }
}
