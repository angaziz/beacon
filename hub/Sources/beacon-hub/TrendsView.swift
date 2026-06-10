import SwiftUI
import BeaconHubKit

// The "Trends" tab (mockup Variant 1): a chart card with a 6h|24h|7d mini selector + pinned pace rows,
// then a cost card scoped to the selected period. Reuses the existing `Module` chrome from DeckUI.swift.
struct TrendsView: View {
    @ObservedObject var model: HubViewModel

    var body: some View {
        VStack(spacing: 10) {
            chartCard
            costCard
        }
    }

    private var chartCard: some View {
        Module {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Text(model.period == .d7 ? "7D WINDOW" : "5H WINDOW")
                        .font(.system(size: 11, weight: .bold)).foregroundStyle(.secondary).tracking(0.5)
                    Spacer()
                    MiniPeriodPicker(period: $model.period)
                }
                UsageChart(claude: claudeSeries, codex: codexSeries, range: chartRange, axisLabels: axisLabels)
                HStack(spacing: 14) {
                    Legend(color: .blue, text: "Claude \(legendPct(model.usage.claude))")
                    Legend(color: .purple, text: "Codex \(legendPct(model.usage.codex))")
                }
                if claudeSeries.segments.isEmpty && codexSeries.segments.isEmpty {
                    Text("line breaks = Hub not running; history starts at install")
                        .font(.system(size: 9)).italic().foregroundStyle(.tertiary)
                }
                PaceRow(name: "Claude", pace: model.claudePace)
                PaceRow(name: "Codex", pace: model.codexPace)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var costCard: some View {
        Module {
            VStack(alignment: .leading, spacing: 8) {
                Text(costScopeLabel).font(.system(size: 11, weight: .bold)).foregroundStyle(.secondary).tracking(0.5)
                HStack {
                    Text("Total").font(.system(size: 12)).foregroundStyle(.secondary)
                    Spacer()
                    Text(String(format: "$%.2f est.", model.cost.totalUSD)).font(.system(size: 12, weight: .semibold))
                }
                ForEach(model.cost.rows, id: \.model) { row in
                    CostRow(row: row, maxCost: maxRowCost)
                }
                Text("est. from transcripts at API list prices - plan is flat-rate")
                    .font(.system(size: 9)).foregroundStyle(.tertiary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    // --- derived ---
    private var chartMetric: ChartMetric { model.period == .d7 ? .d7 : .h5 }
    private var chartRange: ClosedRange<Int> {
        let now = Int(model.now.timeIntervalSince1970)
        let span: Int = { switch model.period { case .h6: return 6*3600; case .h24: return 24*3600; case .d7: return 7*86_400 } }()
        return (now - span)...now
    }
    private var claudeSeries: ChartSeries {
        ChartSeries.build(samples: model.historySamples, provider: .claude, metric: chartMetric, range: chartRange, breakSeconds: 600)
    }
    private var codexSeries: ChartSeries {
        ChartSeries.build(samples: model.historySamples, provider: .codex, metric: chartMetric, range: chartRange, breakSeconds: 600)
    }
    private var axisLabels: [String] {
        let f = DateFormatter()
        switch model.period {
        case .h6, .h24: f.dateFormat = "h a"
        case .d7: f.dateFormat = "MMM d"
        }
        let lo = Date(timeIntervalSince1970: TimeInterval(chartRange.lowerBound))
        let mid = Date(timeIntervalSince1970: TimeInterval((chartRange.lowerBound + chartRange.upperBound) / 2))
        return [f.string(from: lo), f.string(from: mid), "now"]
    }
    private var maxRowCost: Double { model.cost.rows.compactMap { $0.costUSD }.max() ?? 1 }
    private var costScopeLabel: String {
        switch model.period { case .h6: return "COST - LAST 6H"; case .h24: return "COST - TODAY"; case .d7: return "COST - THIS WEEK" }
    }
    private func legendPct(_ p: ProviderUsage) -> String {
        let w = model.period == .d7 ? p.d7 : p.h5
        return w.pct.map { "\($0)%" } ?? "--"
    }
}

private struct MiniPeriodPicker: View {
    @Binding var period: TrendsPeriod
    var body: some View {
        HStack(spacing: 0) {
            ForEach(TrendsPeriod.allCases, id: \.self) { p in
                Text(p.label)
                    .font(.system(size: 10, weight: .semibold))
                    .padding(.vertical, 3).frame(width: 40)
                    .background(period == p ? Color.primary.opacity(0.14) : .clear, in: RoundedRectangle(cornerRadius: 6))
                    .foregroundStyle(period == p ? .primary : .secondary)
                    .contentShape(Rectangle())
                    .onTapGesture { period = p }
            }
        }
        .padding(2)
        .background(Color.primary.opacity(0.06), in: RoundedRectangle(cornerRadius: 8))
    }
}

private struct Legend: View {
    let color: Color; let text: String
    var body: some View {
        HStack(spacing: 5) {
            RoundedRectangle(cornerRadius: 2).fill(color).frame(width: 8, height: 8)
            Text(text).font(.system(size: 10)).foregroundStyle(.secondary)
        }
    }
}

private struct PaceRow: View {
    let name: String; let pace: BurnPaceResult
    var body: some View {
        if let p = pace.pace, let cap = pace.capEpoch, let style = pace.style {
            HStack {
                Text("\(name) pace").font(.system(size: 12)).foregroundStyle(.secondary)
                Spacer()
                Text(text(p, cap, style)).font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(style == .warn ? .orange : .green)
            }
        }
    }
    private func text(_ pace: Double, _ cap: Int, _ style: BurnPaceResult.Style) -> String {
        if style == .safe { return String(format: "+%.0f%%/h - safe past reset", pace) }
        let f = DateFormatter(); f.timeStyle = .short
        return String(format: "+%.0f%%/h - caps ~%@", pace, f.string(from: Date(timeIntervalSince1970: TimeInterval(cap))))
    }
}

private struct CostRow: View {
    let row: ModelCostRow; let maxCost: Double
    var body: some View {
        HStack(spacing: 8) {
            Text(displayName).font(.system(size: 11, weight: .semibold)).frame(width: 86, alignment: .leading)
            Text(tokensText).font(.system(size: 11)).foregroundStyle(.secondary).frame(width: 64, alignment: .leading)
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    Capsule().fill(.primary.opacity(0.10))
                    Capsule().fill(barColor).frame(width: max(2, geo.size.width * fraction))
                }
            }.frame(height: 4)
            Text(costText).font(.system(size: 11, weight: .semibold)).frame(width: 50, alignment: .trailing)
        }
    }
    private var fraction: Double { guard let c = row.costUSD, maxCost > 0 else { return 0 }; return min(1, c / maxCost) }
    private var barColor: Color { row.model.hasPrefix("gpt") ? .purple : .blue }
    private var costText: String { row.costUSD.map { String(format: "$%.2f", $0) } ?? "-" }
    private var tokensText: String {
        if row.tokens >= 1_000_000 { return String(format: "%.1fM tok", Double(row.tokens) / 1_000_000) }
        if row.tokens >= 1_000 { return String(format: "%.0fK tok", Double(row.tokens) / 1_000) }
        return "\(row.tokens) tok"
    }
    private var displayName: String {
        switch row.model {
        case "claude-opus-4-8": return "Opus 4.8"
        case "claude-sonnet-4-6", "sonnet": return "Sonnet 4.6"
        case "claude-haiku-4-5-20251001", "haiku": return "Haiku 4.5"
        case "claude-fable-5", "fable": return "Fable 5"
        case "gpt-5.5": return "GPT-5.5"
        default: return row.model
        }
    }
}
