import AppKit
import Foundation
import ServiceManagement
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
    private let firstRun = FirstRunWindowController()
    private let forgetWindow = ForgetWindowController()
    private let location = LocationProvider()
    private let tickerStore = TickerConfigStore()   // desired ticker list + monotonic rev (issue #92)
    private let tickerSearch = TickerSearch()        // Binance(cached) + Yahoo(live) discovery (issue #92 B4)
    private lazy var tickerEditor = TickerEditorWindowController(model: menubar.viewModel)
    private var binanceCandidates: [TickerCandidate] = []   // warmed-once cache for local Binance filtering

    // Latest known state -- the source of truth we (re)send on heartbeat/reconnect.
    private var usage = Usage(claude: .unavailable, codex: .unavailable)   // merged; resent on heartbeat
    private var buddy = BuddyState()
    private var lastFix: Loc?   // most recent CoreLocation fix (issue #54); rides the (re)connect full frame
    private var heartbeat: Timer?

    // Claude usage has two sources: the oauth poller (works without a CC session, but the endpoint can
    // 429) and Claude Code's statusline rate_limits (authoritative, no token, only while a session runs
    // with the shim). Statusline wins when present. Codex comes from the poller only.
    private var pollerClaude: ProviderUsage = .unavailable
    private var pollerCodex: ProviderUsage = .unavailable
    private var statuslineClaude: ProviderUsage?
    private var statuslineClaudeAt: Date?   // #93: last live statusline POST, for the display-side expiry.
    private var usageErrors: [String] = []
    private var lastUsageErrors: [String] = []   // #59: last value handed to setUsage/BLE, for the equality gate.

    func applicationDidFinishLaunching(_ notification: Notification) {
        startBridge()
        startCentral()
        startPoller()
        startFirstRun()
        startLoginItem()
        startLocation()
        startTickerEditor()

        // Heartbeat resends the full frame WITHOUT loc (issue #54): location rides the (re)connect frame
        // and on-change frames only, never the 30s heartbeat.
        heartbeat = Timer.scheduledTimer(withTimeInterval: 30, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.sendFullFrame(includeLocation: false) }
        }
        heartbeat?.tolerance = 3   // #66 L6: let the OS coalesce the 30s heartbeat wakeup.
    }

    func applicationDidBecomeActive(_ notification: Notification) {
        // Hooks can change out-of-band (manual edit) between launches; re-check on re-focus. The check
        // does sync file IO + JSON parse of ~/.claude/settings.json, so run it off the main thread (#66 L8).
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let ok = HooksInstaller.isInstalled()
            Task { @MainActor in self?.firstRun.setHooks(ok ? .ok : .bad) }
        }
        refreshLoginItem()   // cheap re-sync; the menu-open refresh is the reliable path for this accessory app.
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let bridge else { return .terminateNow }
        var replied = false
        let reply = { if !replied { replied = true; NSApp.reply(toApplicationShouldTerminate: true) } }
        bridge.drainHeldPrompts(reason: "Beacon hub is quitting", completion: reply)
        // Safety cap so Quit never hangs if a socket write stalls; the drain replies earlier on real flush,
        // and immediately when nothing was held. A dropped conn would fail-OPEN per CONTRACT.md C.3.
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { reply() }
        return .terminateLater
    }

    // --- first-run window ---

    private func startFirstRun() {
        firstRun.onInstallHooks = { [weak self] in self?.installHooksAndRefresh() }
        menubar.onOpenSetup = { [weak self] in self?.firstRun.show() }
        firstRun.setHooks(HooksInstaller.isInstalled() ? .ok : .bad)
        firstRun.showIfNeeded()
    }

    // Run the (Process-backed) install off the main thread so the window stays responsive, then hop
    // back to update the row + surface any stderr-derived error.
    private func installHooksAndRefresh() {
        Task.detached { [weak self] in
            let errorMessage: String?
            do { try HooksInstaller.install(); errorMessage = nil }
            catch { errorMessage = error.localizedDescription }
            let installed = HooksInstaller.isInstalled()
            guard let self else { return }
            await MainActor.run {
                self.firstRun.finishInstall(installed: installed ? .ok : .bad, error: errorMessage)
            }
        }
    }

    // --- login item (issue #16) ---

    private func startLoginItem() {
        menubar.onToggleLoginItem = { [weak self] on in self?.applyLoginItem(on) }
        menubar.onMenuWillOpen = { [weak self] in self?.refreshLoginItem() }
        menubar.onForgetDevice = { [weak self] in self?.forgetDevice() }
        refreshLoginItem()
    }

    // Map SMAppService.Status onto the UI enum so MenubarController never imports ServiceManagement.
    private func loginItemStatus() -> MenubarController.LoginItemStatus {
        switch LoginItem.status {
        case .enabled:          return .enabled
        case .requiresApproval: return .requiresApproval
        default:                return .disabled   // .notRegistered/.notFound => off.
        }
    }

    private func refreshLoginItem() { menubar.setLoginItemState(loginItemStatus()) }

    private func applyLoginItem(_ on: Bool) {
        do { try LoginItem.setEnabled(on) }
        catch { showGuidance("Couldn't change the login item", info: error.localizedDescription) }
        refreshLoginItem()   // always re-read truth; never trust the requested value (ad-hoc signing).
        if loginItemStatus() == .requiresApproval {
            showGuidance("Approve Beacon to start at login",
                         info: "Open System Settings > General > Login Items and turn Beacon on.")
        }
    }

    // One-shot informational dialog for a user-initiated action (login-item / forget-device). NOT the
    // persistent menu-bar `alert` slot -- that one is the undeliverable-prompt surface (it appends
    // "couldn't show prompt" and is cleared by reconnect), so reusing it here mis-worded the message and
    // left a stale warning after the user fixed things. A modal dismissed by the user has no stale state.
    private func showGuidance(_ message: String, info: String) {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.alertStyle = .informational
        alert.messageText = message
        alert.informativeText = info
        alert.runModal()
    }

    // --- forget device / re-pair (issue #16) ---

    private func forgetDevice() {
        central.forgetAndRescan()
        // CoreBluetooth cannot clear the OS bond (no API). The app-side reset above just drops the link and
        // rescans, so a healthy bond reconnects on its own. The only real "forget" is the user doing it in
        // Bluetooth settings -- so the window spells out the steps and offers a one-click jump there.
        forgetWindow.onOpenBluetooth = { SettingsLinks.open(SettingsLinks.bluetooth) }
        forgetWindow.show()
    }

    // --- bridge ---

    private func startBridge() {
        let b = ClaudeCodeBridge()
        b.onBuddyUpdate = { [weak self] state in
            Task { @MainActor in self?.onBuddy(state) }
        }
        b.onClaudeUsage = { [weak self] c in
            Task { @MainActor in self?.onStatuslineClaude(c) }
        }
        b.onStatuslineActivity = { [weak self] in
            Task { @MainActor in self?.onStatuslineActivity() }
        }
        b.onPromptUndeliverable = { [weak self] reason in
            Task { @MainActor in self?.menubar.setAlert("Auto-denied: \(reason)") }
        }
        b.onPromptArrived = { [weak self] in
            Task { @MainActor in self?.menubar.playPromptSoundIfEnabled() }
        }
        b.onBridgeStatus = { [weak self] msg in
            Task { @MainActor in self?.menubar.setBridgeAlert(msg) }
        }
        b.start()
        bridge = b
    }

    // --- central ---

    private func startCentral() {
        central.onPhaseChange = { [weak self] phase in
            Task { @MainActor in self?.refreshLink(phase) }
        }
        central.onReady = { [weak self] in
            // Link state is refreshed by the isConnected didSet's onPhaseChange (fires just before
            // this); onReady only resends the full frame to a freshly-(re)subscribed device. The
            // (re)connect frame carries the cached location fix (issue #54). Push the ticker config after
            // the full frame so a rebooted/re-bonded device re-syncs its list (issue #92).
            Task { @MainActor in
                self?.sendFullFrame(includeLocation: true)
                self?.pushTickerConfig()
            }
        }
        central.onCommand = { [weak self] cmd in
            Task { @MainActor in self?.handle(cmd) }
        }
        menubar.onRetryPairing = { [weak self] in self?.central.retryPairing() }
        menubar.onApplyTickerEdit = { [weak self] rows in self?.applyTickerEdit(rows) }
        central.start()
    }

    // `phase` is computed on BeaconCentral's queue (no cross-thread read of the link state).
    private func refreshLink(_ phase: LinkPhase) {
        let link: MenubarController.Link
        switch phase {
        case .bluetoothOff:        link = .bluetoothOff
        case .unauthorized:        link = .unauthorized
        case .unavailable:         link = .unavailable
        case .searching:           link = .searching
        case .connecting(let n):   link = .connecting(n)
        case .connected(let n):    link = .connected(n)
        case .reconnecting:        link = .reconnecting
        case .pairingFailed:       link = .pairingFailed
        }
        menubar.setLink(link)
        let connected: Bool = { if case .connected = phase { return true } else { return false } }()
        if connected {
            menubar.setAlert(nil)   // device reachable again => clear any undeliverable-prompt alert.
        }
        bridge?.setDeviceConnected(connected)
        poller.setDeviceConnected(connected)   // #64: back off the usage poll cadence while disconnected.

        // Drive the first-run window from the SAME phase stream (no second CBCentralManager): Bluetooth
        // is bad only when powered-off/unauthorized/unavailable; paired tracks live .connected.
        switch phase {
        case .bluetoothOff, .unauthorized, .unavailable: firstRun.setBluetooth(.bad)
        default:                                          firstRun.setBluetooth(.ok)
        }
        firstRun.setPaired(connected ? .ok : .bad)
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
        case .configAck(let rev, let ok, let count, let err):
            // Ignore stale acks: a later edit already bumped our rev, so an ack for an older push no
            // longer reflects the desired state we're tracking (issue #92).
            guard rev == tickerStore.current.rev else { break }
            menubar.setTickerSync(ok ? .synced(count ?? tickerStore.current.rows.count)
                                     : .error(err ?? "rejected"))
        case .report:
            // Reassembly handled in a later task (issue #105); parsed frame is dropped here for now.
            break
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

    // Liveness from Claude Code's statusline: fires on EVERY rate_limits POST (#93), even when the value
    // is unchanged. This -- not the deduped value callback -- is what keeps the freshness stamps fresh, so
    // an active-but-flat session never ages the gate out and re-triggers the Keychain poll/prompt.
    // Runs before onStatuslineClaude for the same POST (the bridge fires activity first), so the stamp is
    // set before rebuildUsage reads it.
    private func onStatuslineActivity() {
        poller.noteStatusline()    // #64: a live statusline => skip the (often-429) Claude oauth poll.
        statuslineClaudeAt = Date()
    }

    // Claude usage VALUE from Claude Code's statusline rate_limits (overrides the poller; survives a 429).
    // Deduped (#59); liveness/freshness is handled separately by onStatuslineActivity (#93).
    private func onStatuslineClaude(_ c: ProviderUsage) {
        guard c != statuslineClaude else { return }   // #59: statusline re-fires ~3x/s, usually unchanged.
        statuslineClaude = c
        rebuildUsage()
    }

    private func rebuildUsage() {
        // #93: expire the statusline value on the SAME window the poll-gate uses (UsagePollDecision /
        // poller.pollInterval), so once Claude Code quits and the shim goes silent the display falls back
        // to the poller value within one window instead of pinning the last statusline value -- and stale
        // poller errors stop being masked. The next poller tick (which re-enables the Claude poll on the
        // same window) re-runs this, so display fallback and poll re-enable can't disagree.
        let age = statuslineClaudeAt.map { Date().timeIntervalSince($0) }
        let fresh = UsagePollDecision.statuslineFresh(age: age, interval: poller.pollInterval)
        let usingStatusline = fresh && statuslineClaude != nil
        let claude = (fresh ? statuslineClaude : nil) ?? pollerClaude
        let merged = Usage(claude: claude, codex: pollerCodex)
        // Drop the Claude poller error only while the statusline is actually supplying usage.
        let errors = usingStatusline
            ? usageErrors.filter { !$0.lowercased().contains("claude") }
            : usageErrors
        // #59: skip the BLE frame, the @Published writes, and the lastSync/now restamp on a no-op.
        // The 30s heartbeat still resends the full frame, so a device that missed nothing loses nothing.
        guard merged != usage || errors != lastUsageErrors else { return }
        usage = merged
        lastUsageErrors = errors
        menubar.setUsage(usage, errors: errors)
        sendFrame(StatusFrame(usage: usage))
    }

    private func onBuddy(_ state: BuddyState) {
        guard state != buddy else { return }   // #59: the bridge already gates, but keep AppDelegate self-consistent.
        self.buddy = state
        sendFrame(StatusFrame(buddy: state))
    }

    // --- location ---

    private func startLocation() {
        location.onFix = { [weak self] fix in
            guard let self else { return }
            self.lastFix = fix
            // On-change push: a loc-only frame (the device keeps its usage/buddy). The provider only
            // fires on a meaningful change, so this is naturally throttled.
            self.sendFrame(StatusFrame(loc: fix))
        }
        location.start()
    }

    // --- frame send ---

    // `includeLocation` rides the cached fix on the (re)connect frame but is dropped on the heartbeat.
    private func sendFullFrame(includeLocation: Bool) {
        sendFrame(StatusFrame(usage: usage, buddy: buddy, loc: includeLocation ? lastFix : nil))
    }

    // --- ticker config (issue #92) ---

    // Wire the B4 editor: seed it with the persisted list, warm the Binance universe once for local
    // filtering, route the open action, and provide the merged search hook (Binance local + Yahoo live).
    private func startTickerEditor() {
        menubar.setTickerRows(tickerStore.current.rows)
        menubar.onOpenTickerEditor = { [weak self] in self?.tickerEditor.show() }
        menubar.setTickerSearch { [weak self] query, completion in self?.searchTickers(query, completion) }
        menubar.setTickerValidate { [weak self] row, completion in self?.tickerSearch.validate(row, completion: completion) }

        tickerSearch.fetchBinanceCatalog { [weak self] candidates in
            Task { @MainActor in self?.binanceCandidates = candidates }
        }
    }

    // Local Binance filter merged with a live Yahoo query: fire Yahoo, and in its completion unify it with
    // the already-cached Binance filter. Both deliver on the main actor so the editor mutates @State safely.
    private func searchTickers(_ query: String, _ completion: @escaping ([TickerCandidate]) -> Void) {
        let binance = BinanceCatalog.search(query, in: binanceCandidates)
        tickerSearch.searchYahoo(query) { yahoo in
            Task { @MainActor in completion(TickerMerge.unify(binance: binance, yahoo: yahoo)) }
        }
    }

    // Commit an edit from the menubar editor (B4): persist (bumps rev) then push the new snapshot. Mirror
    // the persisted list back to the view model so the editor reflects exactly what was saved.
    func applyTickerEdit(_ rows: [TickerRow]) {
        tickerStore.save(rows: rows)
        menubar.setTickerRows(tickerStore.current.rows)
        pushTickerConfig()
    }

    // Push the current desired list as ordered chunk frames. Skip when not connected or the list is empty
    // (the firmware rejects an empty assembled snapshot, and ConfigFrame.chunks returns [] for it).
    private func pushTickerConfig() {
        guard central.isConnected, !tickerStore.current.rows.isEmpty else { return }
        do {
            let frames = try ConfigFrame.chunks(rows: tickerStore.current.rows, rev: tickerStore.current.rev)
            for frame in frames { central.send(frame) }
            menubar.setTickerSync(.pending)
        } catch {
            FileHandle.standardError.write(Data("[beacon-hub] ticker config encode failed: \(error.localizedDescription)\n".utf8))
        }
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
