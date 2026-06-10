import SwiftUI

// Observable backing for the Setup window. FirstRunWindowController owns one and writes the live check
// states into it; the SwiftUI SetupPanel renders. Intent closures forward to the controller's side effects.
@MainActor
final class SetupViewModel: ObservableObject {
    @Published var bluetooth: FirstRunWindowController.RowState = .checking
    @Published var hooks: FirstRunWindowController.RowState = .checking
    @Published var paired: FirstRunWindowController.RowState = .checking
    @Published var note: String?
    @Published var installing = false
    @Published var dontShowAgain: Bool

    var onOpenBluetooth: () -> Void = {}
    var onInstall: () -> Void = {}
    var onToggleDontShow: (Bool) -> Void = { _ in }

    init(dontShowAgain: Bool) { self.dontShowAgain = dontShowAgain }
}

struct SetupPanel: View {
    @ObservedObject var model: SetupViewModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            VStack(alignment: .leading, spacing: 3) {
                Text("Set up Beacon").font(.system(size: 15, weight: .semibold))
                Text("Three checks connect the hub to your Mac and the device.")
                    .font(.system(size: 12)).foregroundStyle(.secondary)
            }
            Module(padding: 0) {
                VStack(spacing: 0) {
                    StatusRow(state: model.bluetooth, title: "Bluetooth") {
                        if model.bluetooth != .ok {
                            DeckButton(title: "Open Settings…") { model.onOpenBluetooth() }
                        }
                    }
                    rowDivider
                    StatusRow(state: model.hooks, title: "Claude Code hooks") {
                        if model.installing {
                            DeckButton(title: "Installing…", enabled: false) {}
                        } else if model.hooks != .ok {
                            DeckButton(title: "Install") { model.onInstall() }
                        }
                    }
                    rowDivider
                    StatusRow(state: model.paired, title: "Device connected",
                              hint: "Power on the device; macOS will prompt to pair.") { EmptyView() }
                }
            }
            if let note = model.note {
                NoteLine(text: note, ok: model.hooks == .ok)
            }
            Toggle("Don't show on startup", isOn: Binding(
                get: { model.dontShowAgain },
                set: { model.dontShowAgain = $0; model.onToggleDontShow($0) }))
                .toggleStyle(.checkbox)
                .font(.system(size: 12))
                .foregroundStyle(.secondary)
        }
        .padding(16)
        .frame(width: 380)
    }

    private var rowDivider: some View { Divider().padding(.leading, 42) }
}

// One check: status glyph + title (+ optional hint) on the left, an optional fix button on the right.
struct StatusRow<Trailing: View>: View {
    let state: FirstRunWindowController.RowState
    let title: String
    var hint: String? = nil
    @ViewBuilder var trailing: Trailing

    var body: some View {
        HStack(spacing: 11) {
            Image(systemName: glyph.name).foregroundStyle(glyph.color).frame(width: 18)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.system(size: 13))
                if let hint {
                    Text(hint).font(.system(size: 11)).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer(minLength: 8)
            trailing
        }
        .padding(.horizontal, 13).padding(.vertical, 12)
        .frame(maxWidth: .infinity)
    }

    // Neutral while checking or unsatisfied (the fix button carries the call to action); green only when
    // satisfied. Deliberately no red -- a pending setup step is not an error.
    private var glyph: (name: String, color: Color) {
        switch state {
        case .checking: return ("circle.dashed", .secondary)
        case .ok:       return ("checkmark.circle.fill", .green)
        case .bad:      return ("circle", .secondary)
        }
    }
}

private struct NoteLine: View {
    let text: String
    let ok: Bool
    var body: some View {
        HStack(alignment: .top, spacing: 7) {
            Image(systemName: ok ? "checkmark.circle.fill" : "exclamationmark.circle")
                .font(.system(size: 11)).foregroundStyle(ok ? Color.green : .secondary)
            Text(text).font(.system(size: 11)).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}

#if DEBUG
#Preview {
    let m = SetupViewModel(dontShowAgain: false)
    m.bluetooth = .bad
    m.hooks = .ok
    m.paired = .checking
    m.note = "Hooks installed. Restart Claude Code for them to take effect."
    return SetupPanel(model: m)
}
#endif
