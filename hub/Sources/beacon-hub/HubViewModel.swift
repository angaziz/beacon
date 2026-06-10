import AppKit
import SwiftUI
import BeaconHubKit

// Observable backing store for the SwiftUI HubPanel. MenubarController owns one of these and writes its
// setters into the @Published fields; the panel re-renders. The view never reaches back into AppKit
// business logic -- it calls the intent closures, which MenubarController wires to its own public
// closures / methods. Main-actor only (AppDelegate already marshals every callback onto @MainActor).
// Selected top-level view. Persisted so the popover reopens where the user left it.
enum HubTab: String { case now, trends }
// Chart/cost period. 6h/24h read the 5h-window pct; 7d reads the 7d-window pct (issue #57).
enum TrendsPeriod: String, CaseIterable {
    case h6, h24, d7
    var label: String { switch self { case .h6: return "6h"; case .h24: return "24h"; case .d7: return "7d" } }
}

@MainActor
final class HubViewModel: ObservableObject {
    @Published var link: MenubarController.Link = .searching
    @Published var lastSync: Date?
    @Published var usage = Usage(claude: .unavailable, codex: .unavailable)
    @Published var errors: [String] = []
    @Published var alert: String?          // undeliverable-prompt surface
    @Published var bridgeAlert: String?    // bridge bind failure; priority over alert
    @Published var loginItem: MenubarController.LoginItemStatus = .disabled
    @Published var muted: Bool
    @Published var now: Date

    // --- Trends (issue #57). Persisted selections + cached payloads published by AppDelegate. ---
    @Published var tab: HubTab { didSet { UserDefaults.standard.set(tab.rawValue, forKey: Self.tabKey) } }
    @Published var period: TrendsPeriod {
        didSet {
            UserDefaults.standard.set(period.rawValue, forKey: Self.periodKey)
            onPeriodChange()
        }
    }
    @Published var historySamples: [UsageSample] = []      // raw ring-buffer samples for the chart
    @Published var cost: CostBreakdown = CostBreakdown(rows: [], totalUSD: 0)
    @Published var costLoaded = false   // false until the first background transcript scan publishes
    @Published var claudePace: BurnPaceResult = BurnPaceResult(pace: nil, capEpoch: nil, style: nil)
    @Published var codexPace: BurnPaceResult = BurnPaceResult(pace: nil, capEpoch: nil, style: nil)

    static let tabKey = "BeaconTrendsTab"
    static let periodKey = "BeaconTrendsPeriod"

    // Intent closures, populated by MenubarController (weakly, so VM<->controller is not a retain cycle).
    var onPeriodChange: () -> Void = {}   // fired on period change so AppDelegate can re-scope the cost card.
    var onToggleMute: () -> Void = {}
    var onRequestLoginItem: (Bool) -> Void = { _ in }   // desired on/off; truth re-read async
    var onSetup: () -> Void = {}
    var onForget: () -> Void = {}
    var onRetryPairing: () -> Void = {}
    var onOpenFixURL: () -> Void = {}
    var onQuit: () -> Void = {}

    // `now` seeded once; refreshed on poll + popover open so reset hints stay fresh. `muted` seeded from
    // the same UserDefaults key the controller persists, so the first render matches the real state.
    init(now: Date = Date(), muted: Bool = UserDefaults.standard.bool(forKey: "BeaconPromptSoundMuted")) {
        self.now = now
        self.muted = muted
        self.tab = UserDefaults.standard.string(forKey: Self.tabKey).flatMap(HubTab.init) ?? .now
        self.period = UserDefaults.standard.string(forKey: Self.periodKey).flatMap(TrendsPeriod.init) ?? .h24
    }
}
