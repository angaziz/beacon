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
    private var usage = Usage(claude: .unavailable, codex: .unavailable)
    private var buddy = BuddyState()
    private var heartbeat: Timer?

    private let launchDir = FileManager.default.homeDirectoryForCurrentUser

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
            b.start()
            bridge = b
        } catch {
            FileHandle.standardError.write(Data("[beacon-hub] bridge failed to start: \(error.localizedDescription)\n".utf8))
        }
    }

    // --- central ---

    private func startCentral() {
        central.onStatusChange = { [weak self] in
            Task { @MainActor in self?.refreshLink() }
        }
        central.onReady = { [weak self] in
            Task { @MainActor in self?.refreshLink(); self?.sendFullFrame() }
        }
        central.onCommand = { [weak self] cmd in
            Task { @MainActor in self?.handle(cmd) }
        }
        central.start()
    }

    private func refreshLink() {
        if central.isConnected, let name = central.connectedName {
            menubar.setLink(.connected(name))
        } else {
            menubar.setLink(.scanning)
        }
    }

    private func handle(_ cmd: DeviceCommand) {
        switch cmd {
        case .permission(let id, let approve):
            bridge?.resolve(id: id, approve: approve)
            central.send(HubAck.ack(id: id, ok: true))
        case .launch(let text):
            runLaunch(text)
        }
    }

    // Preset launch (D7): spawn `claude -p <text>` best-effort in the configured working dir.
    private func runLaunch(_ text: String) {
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        proc.arguments = ["claude", "-p", text]
        proc.currentDirectoryURL = launchDir
        do { try proc.run() } catch {
            FileHandle.standardError.write(Data("[beacon-hub] launch failed: \(error.localizedDescription)\n".utf8))
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
        self.usage = usage
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
