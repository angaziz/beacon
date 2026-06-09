import AppKit
import Foundation
import BeaconHubKit

// Wires the four subsystems together: the local hook bridge, the BLE central, and the usage poller all
// feed a single current Usage + BuddyState, which we serialize to a StatusFrame and push to the device.
// We resend the full frame on (re)connect and on a 30 s heartbeat so a freshly-bonded device catches up.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private let menubar = MenubarController()
    private let central = BeaconCentral()
    private var bridge: ClaudeCodeBridge?
    private let poller = UsagePoller()

    // Latest known state -- the source of truth we (re)send on heartbeat/reconnect.
    private var usage = Usage(claude: .unavailable, codex: .unavailable)   // merged; resent on heartbeat
    private var buddy = BuddyState()
    private var heartbeat: Timer?

    // Claude usage has two sources: the oauth poller (works without a CC session, but the endpoint can
    // 429) and Claude Code's statusline rate_limits (authoritative, no token, only while a session runs
    // with the shim). Statusline wins when present. Codex comes from the poller only.
    private var pollerClaude: ProviderUsage = .unavailable
    private var pollerCodex: ProviderUsage = .unavailable
    private var statuslineClaude: ProviderUsage?
    private var usageErrors: [String] = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        startBridge()
        startCentral()
        startPoller()

        heartbeat = Timer.scheduledTimer(withTimeInterval: 30, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.sendFullFrame() }
        }
    }

    // --- bridge ---

    private func startBridge() {
        do {
            let b = try ClaudeCodeBridge()
            b.onBuddyUpdate = { [weak self] state in
                Task { @MainActor in self?.onBuddy(state) }
            }
            b.onClaudeUsage = { [weak self] c in
                Task { @MainActor in self?.onStatuslineClaude(c) }
            }
            b.onPromptUndeliverable = { [weak self] reason in
                Task { @MainActor in self?.menubar.setAlert("Auto-denied: \(reason)") }
            }
            b.start()
            bridge = b
        } catch {
            FileHandle.standardError.write(Data("[beacon-hub] bridge failed to start: \(error.localizedDescription)\n".utf8))
        }
    }

    // --- central ---

    private func startCentral() {
        central.onStatusChange = { [weak self] connected, name in
            Task { @MainActor in self?.refreshLink(connected: connected, name: name) }
        }
        central.onReady = { [weak self] in
            // Link state is refreshed by the isConnected didSet's onStatusChange (fires just before
            // this); onReady only resends the full frame to a freshly-(re)subscribed device.
            Task { @MainActor in self?.sendFullFrame() }
        }
        central.onCommand = { [weak self] cmd in
            Task { @MainActor in self?.handle(cmd) }
        }
        central.start()
    }

    // `connected`/`name` are a snapshot captured on BeaconCentral's queue (no cross-thread read).
    private func refreshLink(connected: Bool, name: String?) {
        if connected, let name = name {
            menubar.setLink(.connected(name))
            menubar.setAlert(nil)   // device reachable again => clear any undeliverable-prompt alert.
        } else {
            menubar.setLink(.scanning)
        }
        bridge?.setDeviceConnected(connected)
    }

    private func handle(_ cmd: DeviceCommand) {
        switch cmd {
        case .permission(let id, let approve):
            // Ack the truth (issue #8): only ok:true when the decision actually applied. A late/
            // superseded decision => ok:false; an id we never minted (or no bridge) => err.
            switch bridge?.resolve(id: id, approve: approve) {
            case .applied:
                central.send(HubAck.ack(id: id, ok: true))
            case .late:
                central.send(HubAck.ack(id: id, ok: false))
            case .unknown, nil:
                central.send(HubAck.err(id: id, reason: "unknown_prompt_id"))
            }
        }
    }

    // --- poller ---

    private func startPoller() {
        poller.onUpdate = { [weak self] usage, errors in
            Task { @MainActor in self?.onUsage(usage, errors) }
        }
        poller.start()
    }

    private func onUsage(_ usage: Usage, _ errors: [String]) {
        pollerClaude = usage.claude
        pollerCodex = usage.codex
        usageErrors = errors
        rebuildUsage()
    }

    // Claude usage from Claude Code's statusline rate_limits (overrides the poller; survives a 429).
    private func onStatuslineClaude(_ c: ProviderUsage) {
        statuslineClaude = c
        rebuildUsage()
    }

    private func rebuildUsage() {
        let claude = statuslineClaude ?? pollerClaude
        usage = Usage(claude: claude, codex: pollerCodex)
        // Drop the Claude poller error once the statusline is supplying usage.
        let errors = statuslineClaude == nil ? usageErrors
            : usageErrors.filter { !$0.lowercased().contains("claude") }
        menubar.setUsage(usage, errors: errors)
        sendFrame(StatusFrame(usage: usage))
    }

    private func onBuddy(_ state: BuddyState) {
        self.buddy = state
        sendFrame(StatusFrame(buddy: state))
    }

    // --- frame send ---

    private func sendFullFrame() {
        sendFrame(StatusFrame(usage: usage, buddy: buddy))
    }

    private func sendFrame(_ frame: StatusFrame) {
        guard central.isConnected else { return }
        do {
            central.send(try frame.encoded())
        } catch {
            FileHandle.standardError.write(Data("[beacon-hub] frame encode failed: \(error.localizedDescription)\n".utf8))
        }
    }
}
