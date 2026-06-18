import AppKit
import SwiftUI

// Dedicated window for the ticker search/editor (issue #92 B4). A transient popover is too cramped for a
// search field + results list + an ordered current-list editor, so this mirrors FirstRunWindowController:
// a lazily-built, reused NSWindow hosting SwiftUI. The app is .accessory, so showing it needs NSApp.activate.
// It observes the SAME HubViewModel the popover panel uses, so tickerSync/tickerRows stay live across both.
@MainActor
final class TickerEditorWindowController: NSObject, NSWindowDelegate {
    private let model: HubViewModel
    private var window: NSWindow?

    init(model: HubViewModel) {
        self.model = model
    }

    func show() {
        let w = window ?? buildWindow()
        window = w
        w.center()
        NSApp.activate(ignoringOtherApps: true)
        w.makeKeyAndOrderFront(nil)
    }

    private func buildWindow() -> NSWindow {
        let host = NSHostingController(rootView: TickerEditorView(model: model))
        let w = NSWindow(contentViewController: host)
        w.styleMask = [.titled, .closable]
        w.title = "Beacon Tickers"
        w.delegate = self
        w.isReleasedWhenClosed = false   // reused across opens
        return w
    }
}
