import Foundation

// Semantic decisions for rendering a usage window: how loud (level), how full (fill), and which
// form the reset timestamp should take. Pure + Foundation-only so the AppKit view stays a thin
// drawer and these stay table-testable. Locale/UI string formatting ("--", "2:40pm", "Fri") lives
// in the view, next to its DateFormatter.

public enum UsageLevel: Equatable {
    case unavailable   // no data (nil pct)
    case normal        // < 70%
    case elevated      // 70...89%
    case critical      // >= 90%
}

// nil => unavailable; otherwise bucketed by pressure.
public func usageLevel(_ pct: Int?) -> UsageLevel {
    guard let pct else { return .unavailable }
    switch pct {
    case ..<70:   return .normal
    case 70..<90: return .elevated
    default:      return .critical
    }
}

// Track fill as 0...1. nil or non-positive => 0; clamped at 1 so a >100 pct can't overflow the bar.
public func usageFillFraction(_ pct: Int?) -> Double {
    guard let pct, pct > 0 else { return 0 }
    return min(Double(pct), 100) / 100
}

// Which reset form to render (the view turns the Date into a localized string):
//   reset <= 0       => .none   (unknown; epoch 0 per Protocol.swift)
//   resetDate <= now => .none   (already elapsed; don't show a misleading past time)
//   within 24h       => .time   (e.g. "2:40pm")
//   beyond 24h       => .weekday (e.g. "Fri")
public enum ResetDisplay: Equatable {
    case none
    case time(Date)
    case weekday(Date)
}

public func resetDisplay(reset: Int, now: Date) -> ResetDisplay {
    guard reset > 0 else { return .none }
    let date = Date(timeIntervalSince1970: TimeInterval(reset))
    if date <= now { return .none }
    if date <= now.addingTimeInterval(24 * 3600) { return .time(date) }
    return .weekday(date)
}
