import AppKit
import SwiftUI

// First-run / Setup window (issue #15): three live checks -- Bluetooth, Claude Code hooks, device
// connected -- each with a one-click fix where one exists. Now rendered by SwiftUI (SetupPanel) in the
// shared Hub Deck style; this controller owns the window + view model and keeps the same public API
// AppDelegate drives. The app is .accessory, so showing this real window needs NSApp.activate.
@MainActor
final class FirstRunWindowController: NSObject, NSWindowDelegate {

    // checking = neutral glyph until the first phase/hooks read resolves a row, so we never flash a
    // failed state before it's known.
    enum RowState { case checking, ok, bad }

    private static let completeKey = "BeaconFirstRunComplete"

    // Install touches files + must re-check, so it routes back through AppDelegate. The Bluetooth row's
    // settings deep link needs no app state, so the view model opens it directly via SettingsLinks.
    var onInstallHooks: (() -> Void)?

    private var window: NSWindow?
    private let model = SetupViewModel(
        dontShowAgain: UserDefaults.standard.bool(forKey: FirstRunWindowController.completeKey))

    func setBluetooth(_ s: RowState) { model.bluetooth = s; maybeComplete() }
    func setHooks(_ s: RowState) { model.hooks = s; maybeComplete() }
    func setPaired(_ s: RowState) { model.paired = s; maybeComplete() }

    func showIfNeeded() {
        guard !UserDefaults.standard.bool(forKey: Self.completeKey) else { return }
        show()
    }

    func show() {
        model.hooks = HooksInstaller.isInstalled() ? .ok : .bad   // re-check on every open (Setup-menu path stays fresh)
        let w = window ?? buildWindow()
        window = w
        model.dontShowAgain = UserDefaults.standard.bool(forKey: Self.completeKey)
        w.center()
        NSApp.activate(ignoringOtherApps: true)
        w.makeKeyAndOrderFront(nil)
    }

    // --- install flow (off the main thread; window stays responsive) ---

    func beginInstall() {
        model.installing = true
        onInstallHooks?()
    }

    // Called back by AppDelegate after the Process completes (it hops to @MainActor).
    func finishInstall(installed: RowState, error: String?) {
        model.installing = false
        model.hooks = installed
        if installed == .ok {
            model.note = "Hooks installed. Restart Claude Code for them to take effect."
        } else if let error = error {
            model.note = error
        }
        maybeComplete()
    }

    private func buildWindow() -> NSWindow {
        model.onOpenBluetooth = { SettingsLinks.open(SettingsLinks.bluetooth) }
        model.onInstall = { [weak self] in self?.beginInstall() }
        model.onToggleDontShow = { on in UserDefaults.standard.set(on, forKey: Self.completeKey) }

        let host = NSHostingController(rootView: SetupPanel(model: model))
        let w = NSWindow(contentViewController: host)
        w.styleMask = [.titled, .closable]
        w.title = "Beacon Setup"
        w.delegate = self
        w.isReleasedWhenClosed = false   // we reuse the window across opens
        return w
    }

    // Set the done flag only on all-ok; the "Don't show again" checkbox is the other path. A plain close
    // never sets it, so an incomplete setup can't silently disappear forever.
    private func maybeComplete() {
        if model.bluetooth == .ok, model.hooks == .ok, model.paired == .ok {
            UserDefaults.standard.set(true, forKey: Self.completeKey)
        }
    }
}
