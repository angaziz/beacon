import AppKit

// Minimal first-run status window (issue #15): three checkmark rows -- Bluetooth, Claude Code hooks,
// device connected -- each reflecting live state pushed in by AppDelegate, with a one-click fix where
// one exists. Programmatic NSWindow (no nib), mirroring MenubarController's hand-built AppKit style.
// The app is .accessory, so showing this real window requires NSApp.activate(ignoringOtherApps:).
@MainActor
final class FirstRunWindowController: NSObject, NSWindowDelegate {

    // checking = neutral glyph until the first phase/hooks read resolves a row, so we never flash a
    // red x before state is known.
    enum RowState { case checking, ok, bad }

    private static let completeKey = "BeaconFirstRunComplete"

    // Install touches files + must re-check, so it routes back through AppDelegate. The settings deep
    // links need no app state, so the Bluetooth row opens them directly via SettingsLinks.
    var onInstallHooks: (() -> Void)?

    private var window: NSWindow?

    private var bluetooth: RowState = .checking { didSet { renderBluetooth() } }
    private var hooks: RowState = .checking { didSet { renderHooks() } }
    private var paired: RowState = .checking { didSet { renderPaired() } }

    private let bluetoothRow = Row(title: "Bluetooth", actionTitle: "Open Settings…")
    private let hooksRow = Row(title: "Claude Code hooks", actionTitle: "Install")
    // "Device connected" reflects LIVE reachability, not a durable bond -- there is no persistent
    // pairing store. No action button: pairing is OS-mediated + passive (auto-scan, prompt on connect).
    private let pairedRow = Row(title: "Device connected", actionTitle: nil,
                                hint: "Power on the device; macOS will prompt to pair.")

    private let noteLabel: NSTextField = {
        let l = FirstRunWindowController.makeLabel("")
        l.textColor = .secondaryLabelColor
        l.font = .systemFont(ofSize: 11)
        l.isHidden = true
        return l
    }()
    private let dontShowAgain = NSButton(checkboxWithTitle: "Don't show again", target: nil, action: nil)

    func setBluetooth(_ s: RowState) { bluetooth = s; maybeComplete() }
    func setHooks(_ s: RowState) { hooks = s; maybeComplete() }
    func setPaired(_ s: RowState) { paired = s; maybeComplete() }

    func showIfNeeded() {
        guard !UserDefaults.standard.bool(forKey: Self.completeKey) else { return }
        show()
    }

    func show() {
        hooks = HooksInstaller.isInstalled() ? .ok : .bad   // re-check on every open (Setup-menu path stays fresh)
        let w = window ?? buildWindow()
        window = w
        dontShowAgain.state = UserDefaults.standard.bool(forKey: Self.completeKey) ? .on : .off
        w.center()
        NSApp.activate(ignoringOtherApps: true)
        w.makeKeyAndOrderFront(nil)
    }

    // --- install flow (off the main thread; window stays responsive) ---

    func beginInstall() {
        hooksRow.action.isEnabled = false
        hooksRow.action.title = "Installing…"
        onInstallHooks?()
    }

    // Called back by AppDelegate after the Process completes (it hops to @MainActor).
    func finishInstall(installed: RowState, error: String?) {
        hooksRow.action.title = "Install"
        hooks = installed
        if installed == .ok {
            showNote("Hooks installed. Restart Claude Code for them to take effect.")
        } else if let error = error {
            showNote(error)
        }
        maybeComplete()
    }

    // --- window construction ---

    private func buildWindow() -> NSWindow {
        let w = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 380, height: 240),
            styleMask: [.titled, .closable],
            backing: .buffered, defer: false)
        w.title = "Beacon Setup"
        w.delegate = self
        w.isReleasedWhenClosed = false   // we reuse the window across opens

        dontShowAgain.target = self
        dontShowAgain.action = #selector(toggleDontShowAgain)

        let stack = NSStackView(views: [
            bluetoothRow.view, hooksRow.view, pairedRow.view, noteLabel, dontShowAgain,
        ])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 12
        stack.translatesAutoresizingMaskIntoConstraints = false

        let content = w.contentView!
        content.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 20),
            stack.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -20),
            stack.topAnchor.constraint(equalTo: content.topAnchor, constant: 20),
        ])

        bluetoothRow.action.target = self; bluetoothRow.action.action = #selector(openBluetoothSettings)
        hooksRow.action.target = self; hooksRow.action.action = #selector(installHooks)

        renderBluetooth(); renderHooks(); renderPaired()
        return w
    }

    private func renderBluetooth() { bluetoothRow.apply(bluetooth) }
    private func renderHooks() { hooksRow.apply(hooks) }
    private func renderPaired() { pairedRow.apply(paired) }

    private func showNote(_ text: String) {
        noteLabel.stringValue = text
        noteLabel.isHidden = false
    }

    // Set the done flag only on all-ok (CODEX MUST-FIX #4); the checkbox is the other path. A plain
    // close never sets it, so an incomplete setup can't silently disappear forever.
    private func maybeComplete() {
        if bluetooth == .ok, hooks == .ok, paired == .ok {
            UserDefaults.standard.set(true, forKey: Self.completeKey)
        }
    }

    @objc private func openBluetoothSettings() {
        // RowState collapses off/unauthorized into .bad; the Bluetooth pane covers both (it surfaces
        // the permission toggle when the app is denied), so a single deep link suffices here.
        SettingsLinks.open(SettingsLinks.bluetooth)
    }

    @objc private func installHooks() { beginInstall() }

    @objc private func toggleDontShowAgain() {
        UserDefaults.standard.set(dontShowAgain.state == .on, forKey: Self.completeKey)
    }

    // --- a single status row: [glyph] title [spacer] action?, plus an optional hint line below ---

    @MainActor
    private final class Row {
        let view = NSStackView()
        let action: NSButton
        private let hasAction: Bool
        private let glyph = NSImageView()
        private let titleLabel: NSTextField

        init(title: String, actionTitle: String?, hint: String? = nil) {
            titleLabel = FirstRunWindowController.makeLabel(title)
            hasAction = actionTitle != nil
            action = NSButton(title: actionTitle ?? "", target: nil, action: nil)
            action.bezelStyle = .rounded
            action.controlSize = .small
            if !hasAction { action.isHidden = true }

            glyph.translatesAutoresizingMaskIntoConstraints = false
            glyph.widthAnchor.constraint(equalToConstant: 18).isActive = true

            let topRow = NSStackView(views: [glyph, titleLabel])
            topRow.orientation = .horizontal
            topRow.spacing = 8
            if hasAction { topRow.addView(action, in: .trailing) }

            view.orientation = .vertical
            view.alignment = .leading
            view.spacing = 2
            view.addView(topRow, in: .top)
            if let hint = hint {
                let hintLabel = FirstRunWindowController.makeLabel(hint)
                hintLabel.textColor = .secondaryLabelColor
                hintLabel.font = .systemFont(ofSize: 11)
                view.addView(hintLabel, in: .top)
            }
        }

        func apply(_ state: RowState) {
            switch state {
            case .checking:
                glyph.image = NSImage(systemSymbolName: "circle.dashed", accessibilityDescription: "checking")
                glyph.contentTintColor = .secondaryLabelColor
            case .ok:
                glyph.image = NSImage(systemSymbolName: "checkmark.circle.fill", accessibilityDescription: "ok")
                glyph.contentTintColor = .systemGreen
            case .bad:
                glyph.image = NSImage(systemSymbolName: "xmark.circle.fill", accessibilityDescription: "needs attention")
                glyph.contentTintColor = .secondaryLabelColor
            }
            // Action button (when the row has one) only makes sense while the row is not satisfied.
            // The paired row has no button -- it stays hidden regardless (the documented exception).
            if hasAction { action.isHidden = state == .ok }
        }
    }

    fileprivate static func makeLabel(_ text: String) -> NSTextField {
        let l = NSTextField(labelWithString: text)
        l.lineBreakMode = .byWordWrapping
        l.maximumNumberOfLines = 0
        return l
    }
}
