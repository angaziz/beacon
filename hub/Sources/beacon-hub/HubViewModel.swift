import AppKit
import SwiftUI
import BeaconHubKit

// Observable backing store for the SwiftUI HubPanel. MenubarController owns one of these and writes its
// setters into the @Published fields; the panel re-renders. The view never reaches back into AppKit
// business logic -- it calls the intent closures, which MenubarController wires to its own public
// closures / methods. Main-actor only (AppDelegate already marshals every callback onto @MainActor).
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

    // Intent closures, populated by MenubarController (weakly, so VM<->controller is not a retain cycle).
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
    }
}
