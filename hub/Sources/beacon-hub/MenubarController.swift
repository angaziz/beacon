import AppKit
import BeaconHubKit

// NSStatusItem + NSMenu. Mirrors link status + last-sync age, the four usage values, any provider
// error strings, a pairing hint, and Quit. Pure display; AppDelegate pushes state in. Main-thread only.
@MainActor
final class MenubarController {
    enum Link { case scanning, connected(String), disconnected }

    private let statusItem: NSStatusItem
    private let menu = NSMenu()

    private var link: Link = .scanning
    private var lastSync: Date?
    private var usage: Usage = Usage(claude: .unavailable, codex: .unavailable)
    private var errors: [String] = []
    private var alert: String?         // persistent loud surface for an undeliverable prompt; nil => none.
    private var bridgeAlert: String?   // bridge bind failure; nil => none. Independent of `alert`.

    private let alertLine = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let statusLine = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let syncLine = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let claudeLine = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let codexLine = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let pairLine = NSMenuItem(title: "Pair: enter the code shown on the device", action: nil, keyEquivalent: "")
    private var errorItems: [NSMenuItem] = []

    init() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        statusItem.button?.title = "Beacon"
        buildMenu()
        render()
    }

    private func buildMenu() {
        // The alert sits at the very top and is hidden until set, so an undeliverable prompt is the
        // first thing the user sees on opening the menu.
        alertLine.isEnabled = false; alertLine.isHidden = true; menu.addItem(alertLine)
        for item in [statusLine, syncLine] { item.isEnabled = false; menu.addItem(item) }
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
        case .scanning: statusLine.title = "Scanning..."
        case .connected(let name): statusLine.title = "Connected \(name)"
        case .disconnected: statusLine.title = "Disconnected"
        }
        statusItem.button?.title = barTitle()

        if let last = lastSync {
            let age = Int(Date().timeIntervalSince(last))
            syncLine.title = "Last sync: \(age)s ago"
        } else {
            syncLine.title = "Last sync: never"
        }

        claudeLine.title = "Claude  5h \(fmt(usage.claude.h5.pct))  7d \(fmt(usage.claude.d7.pct))"
        codexLine.title  = "Codex   5h \(fmt(usage.codex.h5.pct))  7d \(fmt(usage.codex.d7.pct))"

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

    private func barTitle() -> String {
        // An active alert overrides the link state in the bar so it's visible without opening the menu.
        if bridgeAlert != nil || alert != nil { return "Beacon !" }
        switch link {
        case .connected: return "Beacon"
        case .scanning: return "Beacon..."
        case .disconnected: return "Beacon!"
        }
    }

    private func fmt(_ pct: Int?) -> String { pct.map { "\($0)%" } ?? "--" }
}
