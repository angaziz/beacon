import AppKit
import SwiftUI
import BeaconHubKit

// Observable backing store for the SwiftUI HubPanel. MenubarController owns one of these and writes its
// setters into the @Published fields; the panel re-renders. The view never reaches back into AppKit
// business logic -- it calls the intent closures, which MenubarController wires to its own public
// closures / methods. Main-actor only (AppDelegate already marshals every callback onto @MainActor).
// Ack-driven ticker push status (issue #92). idle = nothing pushed yet; pending = chunks sent, awaiting
// the device's config_ack; synced(count) = device applied N rows; error(reason) = device rejected.
enum TickerSyncStatus: Equatable { case idle, pending, synced(Int), error(String) }

// One provider's menubar card state (design 2026-07-19). Capabilities gate which toggles render.
struct ProviderToggle: Identifiable, Equatable {
    let id: String
    let label: String
    let supportsUsage: Bool
    let supportsBuddy: Bool
    var usageOn: Bool
    var buddyOn: Bool
}

@MainActor
final class HubViewModel: ObservableObject {
    @Published var link: MenubarController.Link = .searching
    @Published var lastSync: Date?
    @Published var usage = Usage()   // provider array (design 2026-07-19); rendered as one card per entry
    @Published var notes: [UsageNote] = []   // #108: typed usage notes (info = rate-limited; error = banner)
    @Published var alert: String?          // undeliverable-prompt surface
    @Published var bridgeAlert: String?    // bridge bind failure; priority over alert
    @Published var loginItem: MenubarController.LoginItemStatus = .disabled
    @Published var muted: Bool
    @Published var now: Date
    @Published var tickerSync: TickerSyncStatus = .idle
    @Published var tickerRows: [TickerRow] = []   // issue #92: current desired list, seeds the B4 editor
    // One dynamic card per registered provider (design 2026-07-19): the Usage / Coding Buddy toggles the
    // menubar shows, each gated by the provider's declared capabilities. Backed by ProviderSettings.
    @Published var providers: [ProviderToggle] = []

    // Intent closures, populated by MenubarController (weakly, so VM<->controller is not a retain cycle).
    var onToggleMute: () -> Void = {}
    var onRequestLoginItem: (Bool) -> Void = { _ in }   // desired on/off; truth re-read async
    var onSetup: () -> Void = {}
    var onForget: () -> Void = {}
    var onRetryPairing: () -> Void = {}
    var onApplyTickerEdit: ([TickerRow]) -> Void = { _ in }   // issue #92: B4 editor commits the desired list
    var onOpenTickerEditor: () -> Void = {}                    // issue #92: open the dedicated editor window
    // issue #92: editor calls this with a query; AppDelegate runs Binance(local) + Yahoo(live) and delivers
    // the merged candidates on the main actor. The closure does not retain results; the editor owns them.
    var onSearchTickers: ((String, @escaping ([TickerCandidate]) -> Void) -> Void)?
    // issue #92: editor calls this before adding a candidate; AppDelegate test-fetches the device's data
    // endpoint and delivers (ok, failureReason) on the main actor so non-working symbols are rejected.
    var onValidateTicker: ((TickerRow, @escaping (Bool, String?) -> Void) -> Void)?
    var onOpenFixURL: () -> Void = {}
    var onQuit: () -> Void = {}
    var onSetProviderUsage: (String, Bool) -> Void = { _, _ in }   // provider id, desired on/off (live)
    var onSetProviderBuddy: (String, Bool) -> Void = { _, _ in }

    // `now` seeded once; refreshed on poll + popover open so reset hints stay fresh. `muted` seeded from
    // the same UserDefaults key the controller persists, so the first render matches the real state.
    init(now: Date = Date(), muted: Bool = UserDefaults.standard.bool(forKey: "BeaconPromptSoundMuted")) {
        self.now = now
        self.muted = muted
    }
}
