import AppKit
import SwiftUI

// The single configuration window: provider toggles, per-provider setup, connection checks, and forget
// guidance (folds in the old first-run + forget windows). Mirrors TickerEditorWindowController: a lazily
// built, reused NSWindow hosting SwiftUI, observing the SAME HubViewModel so state stays live. The app is
// .accessory, so showing it needs NSApp.activate. Also owns the first-run auto-open gate.
@MainActor
final class SettingsWindowController: NSObject, NSWindowDelegate {
    static let completeKey = "BeaconFirstRunComplete"

    private let model: HubViewModel
    private var window: NSWindow?

    init(model: HubViewModel) {
        self.model = model
        super.init()
        // The checkbox suppresses the first-run auto-open; it shares the completion flag (checking it is
        // equivalent to "setup done, stop nagging"), matching the old setup window's behavior.
        model.onToggleDontShow = { on in UserDefaults.standard.set(on, forKey: Self.completeKey) }
    }

    // First launch: open until setup completes or the user opts out.
    func showIfNeeded() {
        guard !UserDefaults.standard.bool(forKey: Self.completeKey) else { return }
        show()
    }

    func show() {
        model.dontShowOnStartup = UserDefaults.standard.bool(forKey: Self.completeKey)
        let w = window ?? buildWindow()
        window = w
        w.center()
        NSApp.activate(ignoringOtherApps: true)
        w.makeKeyAndOrderFront(nil)
    }

    // Called when every check passes so the window stops auto-opening on later launches. Never unsets --
    // a manual opt-out via the checkbox is the only other writer and must stick.
    func markComplete() { UserDefaults.standard.set(true, forKey: Self.completeKey) }

    private func buildWindow() -> NSWindow {
        let host = NSHostingController(rootView: SettingsPanel(model: model))
        let w = NSWindow(contentViewController: host)
        w.styleMask = [.titled, .closable]
        w.title = "Beacon Settings"
        w.delegate = self
        w.isReleasedWhenClosed = false   // reused across opens
        return w
    }
}
