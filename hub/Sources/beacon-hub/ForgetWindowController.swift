import AppKit
import SwiftUI

// Forget / re-pair window (issue #16). macOS owns the BLE bond (CoreBluetooth can't clear it), so this is
// guidance: the numbered steps to remove Beacon in System Settings, plus a one-click jump there. Replaces
// the old NSAlert with a SwiftUI surface in the shared Hub Deck style. The app is .accessory, so showing
// this real window needs NSApp.activate.
@MainActor
final class ForgetWindowController: NSObject, NSWindowDelegate {
    var onOpenBluetooth: (() -> Void)?

    private var window: NSWindow?

    func show() {
        let w = window ?? buildWindow()
        window = w
        w.center()
        NSApp.activate(ignoringOtherApps: true)
        w.makeKeyAndOrderFront(nil)
    }

    private func buildWindow() -> NSWindow {
        let panel = ForgetPanel(
            // Open settings AND dismiss -- the user is leaving for System Settings, same as the old alert.
            onOpenBluetooth: { [weak self] in self?.onOpenBluetooth?(); self?.window?.close() },
            onDone: { [weak self] in self?.window?.close() })
        let host = NSHostingController(rootView: panel)
        let w = NSWindow(contentViewController: host)
        w.styleMask = [.titled, .closable]
        w.title = "Forget Beacon"
        w.delegate = self
        w.isReleasedWhenClosed = false
        return w
    }
}

struct ForgetPanel: View {
    var onOpenBluetooth: () -> Void
    var onDone: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            VStack(alignment: .leading, spacing: 3) {
                Text("Forget Beacon").font(.system(size: 15, weight: .semibold))
                Text("macOS owns the pairing, so the device is removed from Bluetooth settings, not here.")
                    .font(.system(size: 12)).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Module(padding: 0) {
                VStack(spacing: 0) {
                    StepRow(1, "Open Bluetooth settings")
                    stepDivider
                    StepRow(2, "Click the info button next to Beacon")
                    stepDivider
                    StepRow(3, "Choose \u{201C}Forget This Device\u{201D}")
                }
            }
            HStack(alignment: .top, spacing: 7) {
                Image(systemName: "info.circle").font(.system(size: 11)).foregroundStyle(.secondary)
                Text("Beacon is now disconnected and searching. Forget it, then bring it back in range to re-pair.")
                    .font(.system(size: 11)).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            HStack(spacing: 9) {
                Spacer()
                DeckButton(title: "Done", large: true) { onDone() }
                DeckButton(title: "Open Bluetooth Settings", kind: .primary, large: true) { onOpenBluetooth() }
            }
        }
        .padding(16)
        .frame(width: 380)
    }

    private var stepDivider: some View { Divider().padding(.leading, 44) }
}

private struct StepRow: View {
    let n: Int
    let text: String
    init(_ n: Int, _ text: String) { self.n = n; self.text = text }

    var body: some View {
        HStack(spacing: 11) {
            Text("\(n)")
                .font(.system(size: 11, weight: .semibold))
                .frame(width: 20, height: 20)
                .background(.primary.opacity(0.1), in: Circle())
            Text(text).font(.system(size: 13))
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 13).padding(.vertical, 12)
        .frame(maxWidth: .infinity)
    }
}

#if DEBUG
#Preview {
    ForgetPanel(onOpenBluetooth: {}, onDone: {})
}
#endif
