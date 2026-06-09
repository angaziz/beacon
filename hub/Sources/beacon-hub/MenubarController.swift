import AppKit
import BeaconHubKit

// NSStatusItem + NSMenu. Mirrors link status + last-sync age, the four usage values, any provider
// error strings, a pairing hint, and Quit. Pure display; AppDelegate pushes state in. Main-thread only.
// NSObject so the `fixLine` remediation item can use Obj-C target-action (#selector(openLink)).
@MainActor
final class MenubarController: NSObject {
    enum Link {
        case bluetoothOff, unauthorized, unavailable
        case searching, connecting(String), connected(String), reconnecting
    }

    private let statusItem: NSStatusItem
    private let menu = NSMenu()

    private var link: Link = .searching
    private var lastSync: Date?
    private var usage: Usage = Usage(claude: .unavailable, codex: .unavailable)
    private var errors: [String] = []
    private var alert: String?         // persistent loud surface for an undeliverable prompt; nil => none.
    private var bridgeAlert: String?   // bridge bind failure; nil => none. Independent of `alert`.

    // URL the fixLine remediation item opens; set per-state in render, read by openLink.
    private var fixURL: URL?

    // Absolute HH:MM avoids a stale relative age with no refresh timer.
    private let timeFormatter: DateFormatter = {
        let f = DateFormatter()
        f.timeStyle = .short
        f.dateStyle = .none
        return f
    }()

    private let alertLine = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let statusLine = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let fixLine = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let syncLine = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let claudeLine = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let codexLine = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let pairLine = NSMenuItem(title: "Pair: enter the code shown on the device", action: nil, keyEquivalent: "")
    private var errorItems: [NSMenuItem] = []

    override init() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        super.init()
        buildMenu()
        render()
    }

    private func buildMenu() {
        // The alert sits at the very top and is hidden until set, so an undeliverable prompt is the
        // first thing the user sees on opening the menu.
        alertLine.isEnabled = false; alertLine.isHidden = true; menu.addItem(alertLine)
        statusLine.isEnabled = false; menu.addItem(statusLine)
        // Remediation link, hidden unless a fault state (bluetoothOff/unauthorized) sets a fixURL.
        fixLine.target = self; fixLine.action = #selector(openLink); fixLine.isHidden = true
        menu.addItem(fixLine)
        syncLine.isEnabled = false; menu.addItem(syncLine)
        menu.addItem(.separator())
        for item in [claudeLine, codexLine] { item.isEnabled = false; menu.addItem(item) }
        menu.addItem(.separator())
        pairLine.isEnabled = false
        menu.addItem(pairLine)
        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: "Quit Beacon", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
        statusItem.menu = menu
    }

    func setLink(_ link: Link) { self.link = link; render() }
    func setAlert(_ message: String?) { self.alert = message; render() }
    func setBridgeAlert(_ message: String?) { self.bridgeAlert = message; render() }
    func setUsage(_ usage: Usage, errors: [String]) {
        self.usage = usage
        self.errors = errors
        self.lastSync = Date()
        render()
    }

    private func render() {
        // Bridge alert takes priority (safety-critical). One reused alertLine, so each branch sets its
        // title state explicitly -- clearing attributedTitle on the others so stale red can't leak.
        if let bridgeAlert = bridgeAlert {
            alertLine.attributedTitle = NSAttributedString(
                string: "! \(bridgeAlert)",
                attributes: [.foregroundColor: NSColor.systemRed])
            alertLine.isHidden = false
        } else if let alert = alert {
            alertLine.attributedTitle = nil
            alertLine.title = "! \(alert) -- couldn't show prompt"
            alertLine.isHidden = false
        } else {
            alertLine.attributedTitle = nil
            alertLine.isHidden = true
        }

        switch link {
        case .bluetoothOff:        statusLine.title = "Bluetooth is off"
        case .unauthorized:        statusLine.title = "Bluetooth permission needed"
        case .unavailable:         statusLine.title = "Bluetooth unavailable"
        case .searching:           statusLine.title = "Searching for device…"
        case .connecting(let n):   statusLine.title = "Connecting to \(n)…"
        case .connected(let n):    statusLine.title = "Connected \(n)"
        case .reconnecting:        statusLine.title = "Disconnected — reconnecting"
        }

        // Two distinct remediations; the URL is stored so a single openLink selector serves both.
        switch link {
        case .bluetoothOff:
            fixLine.title = "Open Bluetooth settings…"
            // macOS 13+ pane id; the pre-Ventura "com.apple.Bluetooth" no longer resolves.
            fixURL = URL(string: "x-apple.systempreferences:com.apple.BluetoothSettings")
            fixLine.isEnabled = true; fixLine.isHidden = false
        case .unauthorized:
            fixLine.title = "Open Privacy settings…"
            fixURL = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Bluetooth")
            fixLine.isEnabled = true; fixLine.isHidden = false
        default:
            fixURL = nil
            fixLine.isEnabled = false; fixLine.isHidden = true
        }

        applyBarIcon()

        if let last = lastSync {
            syncLine.title = "Last sync: \(timeFormatter.string(from: last))"
        } else {
            syncLine.title = "Last sync: never"
        }

        claudeLine.title = "Claude  5h \(fmt(usage.claude.h5.pct))  7d \(fmt(usage.claude.d7.pct))"
        codexLine.title  = "Codex   5h \(fmt(usage.codex.h5.pct))  7d \(fmt(usage.codex.d7.pct))"

        // Pairing hint is only actionable while looking for / connecting to a device.
        switch link {
        case .searching, .connecting: pairLine.isHidden = false
        default: pairLine.isHidden = true
        }

        // Rebuild the error block in place (above the pairing hint).
        for item in errorItems { menu.removeItem(item) }
        errorItems.removeAll()
        guard !errors.isEmpty else { return }
        var at = menu.index(of: pairLine)
        let sep = NSMenuItem.separator()
        menu.insertItem(sep, at: at); at += 1
        errorItems.append(sep)
        for e in errors {
            let item = NSMenuItem(title: "! \(e)", action: nil, keyEquivalent: "")
            item.isEnabled = false
            menu.insertItem(item, at: at); at += 1
            errorItems.append(item)
        }
    }

    // contentTintColor must be assigned (color OR nil) on EVERY call: AppKit only tints template images,
    // and a stale tint from a fault/working render would otherwise leak into a later neutral/connected one.
    private func applyBarIcon() {
        guard let button = statusItem.button else { return }

        // An active alert overrides the link state so it's visible without opening the menu.
        let symbol: String
        let tint: NSColor?
        let description: String
        if bridgeAlert != nil || alert != nil {
            symbol = "exclamationmark.triangle.fill"; tint = .systemRed; description = "Beacon: alert"
        } else {
            switch link {
            case .bluetoothOff:
                symbol = "exclamationmark.triangle.fill"; tint = .systemOrange; description = "Beacon: Bluetooth off"
            case .unauthorized:
                symbol = "exclamationmark.triangle.fill"; tint = .systemOrange; description = "Beacon: permission needed"
            case .unavailable:
                symbol = "exclamationmark.triangle.fill"; tint = .systemRed; description = "Beacon: Bluetooth unavailable"
            case .searching:
                symbol = "antenna.radiowaves.left.and.right"; tint = .secondaryLabelColor; description = "Beacon: searching"
            case .connecting:
                symbol = "antenna.radiowaves.left.and.right"; tint = .secondaryLabelColor; description = "Beacon: connecting"
            case .reconnecting:
                symbol = "antenna.radiowaves.left.and.right"; tint = .secondaryLabelColor; description = "Beacon: reconnecting"
            case .connected:
                symbol = "antenna.radiowaves.left.and.right"; tint = nil; description = "Beacon: connected"
            }
        }

        button.title = ""
        let image = NSImage(systemSymbolName: symbol, accessibilityDescription: description)
        image?.isTemplate = true
        button.image = image
        button.contentTintColor = tint
    }

    @objc private func openLink() {
        // Fall back to System Settings root if the pane id has drifted and the deep link fails to open.
        let fallback = URL(string: "x-apple.systempreferences:")!
        if let url = fixURL, NSWorkspace.shared.open(url) { return }
        NSWorkspace.shared.open(fallback)
    }

    private func fmt(_ pct: Int?) -> String { pct.map { "\($0)%" } ?? "--" }
}
