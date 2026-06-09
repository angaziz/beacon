import AppKit
import BeaconHubKit

// One provider's usage block in the menu: provider name + two windows (5h, 7d), each a labeled
// rounded progress bar with a pct and a reset hint. Pure display -- MenubarController owns the data
// and calls update(_:now:). Hosted in a disabled NSMenuItem (non-interactive), so there is no click
// or highlight handling. Semantic decisions (level/fill/reset form) come from BeaconHubKit; this view
// only maps them to colors and localized strings and draws.
final class UsageRowView: NSView {
    private let provider: String
    private var usage: ProviderUsage = .unavailable
    private var now = Date()

    private let font = NSFont.menuFont(ofSize: 0)
    private let nameFont = NSFont.boldSystemFont(ofSize: NSFont.menuFont(ofSize: 0).pointSize)

    // Geometry. leftPad lines the block up with the menu's indented text rows; the rest is tuned so
    // label + bar + pct + reset fit on one line at the default width and reflow if the menu stretches.
    private let leftPad: CGFloat = 21
    private let rightPad: CGFloat = 14
    private let vPad: CGFloat = 5
    private let rowGap: CGFloat = 3
    private let labelW: CGFloat = 24     // "5h" / "7d"
    private let pctW: CGFloat = 38       // "100%"
    private let resetW: CGFloat = 66     // "resets Fri"
    private let gap: CGFloat = 6
    private let barH: CGFloat = 6

    private let timeFormatter: DateFormatter = {
        let f = DateFormatter(); f.timeStyle = .short; f.dateStyle = .none; return f
    }()
    private let weekdayFormatter: DateFormatter = {
        let f = DateFormatter(); f.dateFormat = "EEE"; return f
    }()

    init(provider: String) {
        self.provider = provider
        super.init(frame: NSRect(x: 0, y: 0, width: 260, height: 0))
        autoresizingMask = [.width]
        setAccessibilityRole(.group)
        frame.size.height = vPad * 2 + lineHeight * 3 + rowGap * 2
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) unused") }

    override var isFlipped: Bool { true }   // lay out top-to-bottom

    private var lineHeight: CGFloat { font.ascender - font.descender + font.leading }

    func update(_ usage: ProviderUsage, now: Date) {
        self.usage = usage
        self.now = now
        setAccessibilityValue(spokenSummary)
        needsDisplay = true
    }

    override func viewDidChangeEffectiveAppearance() { needsDisplay = true }

    override func draw(_ dirtyRect: NSRect) {
        var y = vPad
        draw(text: provider, font: nameFont, color: .secondaryLabelColor,
             in: NSRect(x: leftPad, y: y, width: bounds.width - leftPad - rightPad, height: lineHeight))
        y += lineHeight + rowGap
        drawWindow("5h", usage.h5, atY: y)
        y += lineHeight + rowGap
        drawWindow("7d", usage.d7, atY: y)
    }

    private func drawWindow(_ label: String, _ w: UsageWindow, atY y: CGFloat) {
        let rowRect = NSRect(x: leftPad, y: y, width: bounds.width - leftPad - rightPad, height: lineHeight)
        draw(text: label, font: font, color: .secondaryLabelColor,
             in: NSRect(x: rowRect.minX, y: y, width: labelW, height: lineHeight))

        let barX = rowRect.minX + labelW + gap
        let barW = max(0, rowRect.maxX - resetW - pctW - 2 * gap - barX)
        let barRect = NSRect(x: barX, y: y + (lineHeight - barH) / 2, width: barW, height: barH)
        drawBar(barRect, level: usageLevel(w.pct), fraction: usageFillFraction(w.pct))

        let pctRect = NSRect(x: barRect.maxX + gap, y: y, width: pctW, height: lineHeight)
        draw(text: pctText(w.pct), font: font, color: .labelColor, in: pctRect, align: .right)

        let resetRect = NSRect(x: pctRect.maxX + gap, y: y, width: resetW, height: lineHeight)
        draw(text: resetText(w.reset), font: font, color: .secondaryLabelColor, in: resetRect, align: .right)
    }

    private func drawBar(_ rect: NSRect, level: UsageLevel, fraction: Double) {
        let radius = rect.height / 2
        NSColor.quaternaryLabelColor.setFill()
        NSBezierPath(roundedRect: rect, xRadius: radius, yRadius: radius).fill()
        guard fraction > 0, let color = fillColor(level) else { return }
        let fillRect = NSRect(x: rect.minX, y: rect.minY, width: rect.width * fraction, height: rect.height)
        color.setFill()
        NSBezierPath(roundedRect: fillRect, xRadius: radius, yRadius: radius).fill()
    }

    private func fillColor(_ level: UsageLevel) -> NSColor? {
        switch level {
        case .unavailable: return nil
        case .normal:      return .systemGreen
        case .elevated:    return .systemOrange
        case .critical:    return .systemRed
        }
    }

    private func pctText(_ pct: Int?) -> String { pct.map { "\($0)%" } ?? "--" }

    private func resetText(_ reset: Int) -> String {
        switch resetDisplay(reset: reset, now: now) {
        case .none:                return ""
        case .time(let d):         return "resets \(timeFormatter.string(from: d))"
        case .weekday(let d):      return "resets \(weekdayFormatter.string(from: d))"
        }
    }

    private func draw(text: String, font: NSFont, color: NSColor, in rect: NSRect,
                      align: NSTextAlignment = .left) {
        let para = NSMutableParagraphStyle()
        para.alignment = align
        para.lineBreakMode = .byTruncatingTail
        let attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: color,
                                                     .paragraphStyle: para]
        let textH = font.ascender - font.descender
        let centered = NSRect(x: rect.minX, y: rect.minY + (rect.height - textH) / 2,
                              width: rect.width, height: textH)
        (text as NSString).draw(in: centered, withAttributes: attrs)
    }

    private var spokenSummary: String {
        func part(_ name: String, _ w: UsageWindow) -> String {
            let pct = w.pct.map { "\($0) percent" } ?? "unavailable"
            let reset = resetText(w.reset)
            return reset.isEmpty ? "\(name) \(pct)" : "\(name) \(pct), \(reset)"
        }
        return "\(provider). \(part("5 hour", usage.h5)); \(part("7 day", usage.d7))."
    }
}
