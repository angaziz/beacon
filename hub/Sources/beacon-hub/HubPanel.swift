import SwiftUI
import BeaconHubKit

// The Hub Deck panel (design 4A): a Control-Center-style surface hosted in the status-bar NSPopover.
// Pure presentation over HubViewModel -- it maps link/usage state to native controls and calls the
// view model's intent closures. Semantic decisions (level, fill fraction, reset form) come from
// BeaconHubKit; this view only turns them into colors, localized strings, and layout.
struct HubPanel: View {
    @ObservedObject var model: HubViewModel
    var closeAndRun: (@escaping () -> Void) -> Void   // dismiss the popover, then run the action

    var body: some View {
        VStack(spacing: 10) {
            if let banner = model.bridgeAlert ?? model.alert.map({ "\($0) — couldn't show prompt" }) {
                Banner(text: banner)
            }
            HeaderModule(model: model, closeAndRun: closeAndRun)
            if !model.errors.isEmpty {
                Module {
                    VStack(alignment: .leading, spacing: 4) {
                        ForEach(model.errors, id: \.self) { Label($0, systemImage: "exclamationmark.triangle.fill") }
                    }
                    .font(.system(size: 11))
                    .foregroundStyle(.red)
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            HStack(spacing: 10) {
                ProviderCard(name: "Claude", usage: model.usage.claude, now: model.now)
                ProviderCard(name: "Codex", usage: model.usage.codex, now: model.now)
            }
            TogglesModule(model: model)
            ActionBar(model: model, closeAndRun: closeAndRun)
        }
        .padding(12)
        .frame(width: 340)
    }
}

// MARK: - Header

private struct HeaderModule: View {
    @ObservedObject var model: HubViewModel
    var closeAndRun: (@escaping () -> Void) -> Void

    var body: some View {
        Module {
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 10) {
                    ZStack {
                        Circle().fill(.blue).frame(width: 30, height: 30)
                        Image(systemName: "wave.3.right").font(.system(size: 13, weight: .semibold)).foregroundStyle(.white)
                    }
                    VStack(alignment: .leading, spacing: 1) {
                        Text(deviceName).font(.system(size: 13, weight: .semibold))
                        Text(statusText).font(.system(size: 11)).foregroundStyle(.secondary)
                        Text(syncText).font(.system(size: 11)).foregroundStyle(.secondary)
                    }
                    Spacer()
                    if isConnected {
                        Circle().fill(.green).frame(width: 8, height: 8)
                    }
                }
                if showPairingHint {
                    Text("Pair: enter the code shown on the device")
                        .font(.system(size: 11)).foregroundStyle(.secondary)
                }
                if let fix = fixLabel {
                    LinkButton(fix, systemImage: "gearshape") { closeAndRun(model.onOpenFixURL) }
                }
                if case .pairingFailed = model.link {
                    LinkButton("Try pairing again", systemImage: "arrow.clockwise") { closeAndRun(model.onRetryPairing) }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var deviceName: String {
        switch model.link {
        case .connected(let n), .connecting(let n): return n
        default: return "Beacon"
        }
    }
    private var isConnected: Bool { if case .connected = model.link { return true }; return false }
    private var statusText: String {
        switch model.link {
        case .bluetoothOff:      return "Bluetooth is off"
        case .unauthorized:      return "Bluetooth permission needed"
        case .unavailable:       return "Bluetooth unavailable"
        case .searching:         return "Searching for device…"
        case .connecting(let n): return "Connecting to \(n)…"
        case .connected:         return "Connected"
        case .reconnecting:      return "Disconnected — reconnecting"
        case .pairingFailed:     return "Pairing failed"
        }
    }
    private var syncText: String {
        guard let last = model.lastSync else { return "Last sync: never" }
        return "Last sync: \(Self.time.string(from: last))"
    }
    private var showPairingHint: Bool {
        switch model.link {
        case .searching, .connecting, .pairingFailed: return true
        default: return false
        }
    }
    private var fixLabel: String? {
        switch model.link {
        case .bluetoothOff: return "Open Bluetooth settings…"
        case .unauthorized: return "Open Privacy settings…"
        default:            return nil
        }
    }

    static let time: DateFormatter = { let f = DateFormatter(); f.timeStyle = .short; f.dateStyle = .none; return f }()
}

// MARK: - Usage

private struct ProviderCard: View {
    let name: String
    let usage: ProviderUsage
    let now: Date

    var body: some View {
        Module {
            VStack(alignment: .leading, spacing: 9) {
                Text(name).font(.system(size: 12, weight: .semibold))
                WindowRow(label: "5h", window: usage.h5, now: now)
                WindowRow(label: "7d", window: usage.d7, now: now)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel(name)
        .accessibilityValue(summary)
    }

    private var summary: String {
        func part(_ unit: String, _ w: UsageWindow) -> String {
            let pct = w.pct.map { "\($0) percent" } ?? "unavailable"
            let reset = WindowRow.resetText(w.reset, now: now)
            return reset.isEmpty ? "\(unit) \(pct)" : "\(unit) \(pct), \(reset)"
        }
        return "\(part("5 hour", usage.h5)); \(part("7 day", usage.d7))."
    }
}

private struct WindowRow: View {
    let label: String
    let window: UsageWindow
    let now: Date

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(label).font(.system(size: 10, weight: .semibold)).foregroundStyle(.secondary)
                Spacer()
                Text(Self.resetText(window.reset, now: now)).font(.system(size: 10)).foregroundStyle(.secondary)
            }
            Text(pctText)
                .font(.system(size: 21, weight: .bold)).monospacedDigit()
                .foregroundStyle(color ?? .secondary)
            LevelBar(fraction: usageFillFraction(window.pct), color: color)
        }
    }

    private var color: Color? { Self.color(usageLevel(window.pct)) }
    private var pctText: String { window.pct.map { "\($0)%" } ?? "--" }

    static func color(_ level: UsageLevel) -> Color? {
        switch level {
        case .unavailable: return nil
        case .normal:      return .green
        case .elevated:    return .orange
        case .critical:    return .red
        }
    }

    // Localized reset hint, matching the old UsageRowView: "resets 9:40 AM" within 24h, "resets Mon" beyond.
    static func resetText(_ reset: Int, now: Date) -> String {
        switch resetDisplay(reset: reset, now: now) {
        case .none:           return ""
        case .time(let d):    return "resets \(time.string(from: d))"
        case .weekday(let d): return "resets \(weekday.string(from: d))"
        }
    }
    static let time: DateFormatter = { let f = DateFormatter(); f.timeStyle = .short; f.dateStyle = .none; return f }()
    static let weekday: DateFormatter = { let f = DateFormatter(); f.dateFormat = "EEE"; return f }()
}

private struct LevelBar: View {
    let fraction: Double
    let color: Color?

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Capsule().fill(.primary.opacity(0.14))
                if fraction > 0, let color {
                    Capsule().fill(color).frame(width: max(2, geo.size.width * fraction))
                }
            }
        }
        .frame(height: 4)
    }
}

// MARK: - Toggles

private struct TogglesModule: View {
    @ObservedObject var model: HubViewModel

    var body: some View {
        Module(padding: 0) {
            VStack(spacing: 0) {
                ToggleRow(icon: "speaker.slash.fill", title: "Mute prompt sound", isOn: muteBinding)
                Divider().padding(.leading, 12)
                ToggleRow(icon: "person.fill", title: "Start at login",
                          subtitle: model.loginItem == .requiresApproval ? "Approve in Login Items" : nil,
                          isOn: loginBinding)
            }
        }
    }

    private var muteBinding: Binding<Bool> {
        Binding(get: { model.muted }, set: { model.muted = $0; model.onToggleMute() })
    }
    // No optimistic flip: request the opposite of the re-read truth; the UI only changes when
    // setLoginItemState writes back model.loginItem (ad-hoc signing can land on .requiresApproval).
    private var loginBinding: Binding<Bool> {
        Binding(get: { model.loginItem == .enabled }, set: { _ in model.onRequestLoginItem(model.loginItem != .enabled) })
    }
}

// Text + icon pinned left, switch pinned right (built-in Toggle layout centered the label).
private struct ToggleRow: View {
    let icon: String
    let title: String
    var subtitle: String? = nil
    let isOn: Binding<Bool>

    var body: some View {
        HStack(spacing: 9) {
            Image(systemName: icon).frame(width: 16).foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: 1) {
                Text(title).font(.system(size: 13))
                if let subtitle {
                    Text(subtitle).font(.system(size: 10)).foregroundStyle(.secondary)
                }
            }
            Spacer(minLength: 8)
            Toggle("", isOn: isOn).labelsHidden().toggleStyle(.switch)
        }
        .padding(.horizontal, 12).padding(.vertical, 9)
        .frame(maxWidth: .infinity)
    }
}

// MARK: - Actions

private struct ActionBar: View {
    @ObservedObject var model: HubViewModel
    var closeAndRun: (@escaping () -> Void) -> Void

    var body: some View {
        HStack(spacing: 10) {
            ActionButton("Setup…", systemImage: "gearshape") { closeAndRun(model.onSetup) }
            ActionButton("Forget / re-pair", systemImage: "arrow.triangle.2.circlepath") { closeAndRun(model.onForget) }
            ActionButton("Quit Beacon", systemImage: "power", tint: .red) { closeAndRun(model.onQuit) }
        }
    }
}

// MARK: - Shared chrome

private struct Banner: View {
    let text: String
    var body: some View {
        Label(text, systemImage: "exclamationmark.triangle.fill")
            .font(.system(size: 12, weight: .medium))
            .foregroundStyle(.white)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(11)
            .background(.red, in: RoundedRectangle(cornerRadius: 13, style: .continuous))
    }
}

private struct LinkButton: View {
    let title: String
    let systemImage: String
    let action: () -> Void
    init(_ title: String, systemImage: String, action: @escaping () -> Void) {
        self.title = title; self.systemImage = systemImage; self.action = action
    }
    var body: some View {
        Button(action: action) {
            Label(title, systemImage: systemImage).font(.system(size: 11, weight: .medium))
        }
        .buttonStyle(.plain)
        .foregroundStyle(.blue)
    }
}

private struct ActionButton: View {
    let title: String
    let systemImage: String
    var tint: Color = .primary
    let action: () -> Void
    init(_ title: String, systemImage: String, tint: Color = .primary, action: @escaping () -> Void) {
        self.title = title; self.systemImage = systemImage; self.tint = tint; self.action = action
    }
    var body: some View {
        Button(action: action) {
            VStack(spacing: 5) {
                Image(systemName: systemImage).font(.system(size: 16)).foregroundStyle(tint)
                Text(title).font(.system(size: 10.5)).foregroundStyle(.secondary).lineLimit(1)
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 11)
            .background(.primary.opacity(0.06), in: RoundedRectangle(cornerRadius: 13, style: .continuous))
        }
        .buttonStyle(.plain)
    }
}

#if DEBUG
#Preview {
    let m = HubViewModel(now: Date(timeIntervalSince1970: 1_733_800_000))
    m.link = .connected("Beacon-8428")
    m.lastSync = Date(timeIntervalSince1970: 1_733_800_000)
    m.usage = Usage(
        claude: ProviderUsage(h5: UsageWindow(pct: 2, reset: 1_733_820_000),
                              d7: UsageWindow(pct: 0, reset: 1_734_200_000)),
        codex: ProviderUsage(h5: UsageWindow(pct: 1, reset: 1_733_821_000),
                             d7: UsageWindow(pct: 93, reset: 1_734_300_000)))
    return HubPanel(model: m, closeAndRun: { $0() })
}
#endif
