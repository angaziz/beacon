import Foundation

// The three period scopes the cost card supports. last6h is a rolling window; day/week are local
// calendar boundaries ("today" / "this week"). Mapping to chart periods: 6h->last6h, 24h->day, 7d->week.
public enum CostPeriod {
    case last6h, day, week
}

// Aggregates deduped transcript turns into a flat, deduped turn list, then prices period queries on
// demand. Calendar is injected so midnight/week boundaries are deterministic in tests. Cost is computed
// per provider so cache mechanics stay correct; tokens count raw.
public struct CostAggregator {
    private let calendar: Calendar
    private var seenKeys = Set<String>()

    // Flat, deduped turn records — exact for any time range. (Corpus is low-thousands of turns; linear
    // scans on popover-period changes are trivial and run off the UI path anyway.)
    private struct Turn { let model: String; let epoch: Int; let tokens: Int; let cost: Double; let priced: Bool }
    private var turns: [Turn] = []

    public init(calendar: Calendar) { self.calendar = calendar }

    public mutating func addClaude(_ turn: ClaudeTurn) {
        guard seenKeys.insert(turn.dedupKey).inserted else { return }
        let priced = PricingTable.shared.pricing(for: turn.model)
        let cost = priced.map { CostMath.cost(claude: turn.usage, pricing: $0) } ?? 0
        turns.append(Turn(model: turn.model, epoch: turn.epochSeconds, tokens: turn.usage.totalTokens,
                          cost: cost, priced: priced != nil))
    }

    public mutating func addCodex(model: String, turn: CodexTurn) {
        // Codex token_count events have no per-event id; dedup is by file cursor in the reader, not here.
        let priced = PricingTable.shared.pricing(for: model)
        let cost = priced.map { CostMath.cost(codex: turn.usage, pricing: $0) } ?? 0
        turns.append(Turn(model: model, epoch: turn.epochSeconds, tokens: turn.usage.totalTokens,
                          cost: cost, priced: priced != nil))
    }

    private func range(for period: CostPeriod, now: Int) -> ClosedRange<Int> {
        let nowDate = Date(timeIntervalSince1970: TimeInterval(now))
        switch period {
        case .last6h:
            return (now - 6*3600)...now
        case .day:
            let start = Int(calendar.startOfDay(for: nowDate).timeIntervalSince1970)
            return start...now
        case .week:
            let comps = calendar.dateComponents([.yearForWeekOfYear, .weekOfYear], from: nowDate)
            let start = calendar.date(from: comps).map { Int($0.timeIntervalSince1970) } ?? now
            return start...now
        }
    }

    public func breakdown(period: CostPeriod, now: Int, table: PricingTable) -> CostBreakdown {
        let r = range(for: period, now: now)
        var byModel: [String: (tokens: Int, cost: Double, priced: Bool)] = [:]
        for t in turns where t.epoch >= r.lowerBound && t.epoch <= r.upperBound {
            var acc = byModel[t.model] ?? (0, 0, true)
            acc.tokens += t.tokens; acc.cost += t.cost; acc.priced = acc.priced && t.priced
            byModel[t.model] = acc
        }
        var rows: [ModelCostRow] = []
        var total = 0.0
        for (model, acc) in byModel {
            rows.append(ModelCostRow(model: model, tokens: acc.tokens, costUSD: acc.priced ? acc.cost : nil))
            if acc.priced { total += acc.cost }
        }
        rows.sort { ($0.costUSD ?? -1) > ($1.costUSD ?? -1) }
        return CostBreakdown(rows: rows, totalUSD: total)
    }
}
