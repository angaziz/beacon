import AppKit
import Foundation
import ServiceManagement
import BeaconHubKit

// Wires the subsystems together (design 2026-07-19): a shared LocalIngestServer + registered
// AgentProviders (Claude, Codex) feed a ProviderMux, which merges per-provider usage/sessions/prompts
// into a single Usage + BuddyState + [Session]. We serialize those to StatusFrame/SessionsFrame and push
// to the device over BLE, resending the full frame on (re)connect and on a 30 s heartbeat. The usage
// poller iterates usage-enabled providers; per-provider toggles (ProviderSettings) drive live setEnabled.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private let menubar = MenubarController()
    private let central = BeaconCentral()
    private let mux = ProviderMux()
    private let settings = ProviderSettings(store: UserDefaults.standard)
    private let ingest = LocalIngestServer()
    private var providers: [AgentProvider] = []
    private var claude: ClaudeCodeProvider?            // typed ref for drain + device-connected + statusline
    private var codex: CodexProvider?                  // typed ref for drain + device-connected
    private var poller: UsagePoller!                   // built once providers exist
    private let location = LocationProvider()
    private let tickerStore = TickerConfigStore()   // desired ticker list + monotonic rev (issue #92)
    private var reportAssembler = ReportAssembler()   // reassembles device->hub ticker report chunks (#105)
    private let tickerSearch = TickerSearch()        // Binance(cached) + Yahoo(live) discovery (issue #92 B4)
    private lazy var tickerEditor = TickerEditorWindowController(model: menubar.viewModel)
    private lazy var settingsWindow = SettingsWindowController(model: menubar.viewModel)
    private var binanceCandidates: [TickerCandidate] = []   // warmed-once cache for local Binance filtering

    // A single ephemeral session (15s timeout) shared by every provider's usage source (#64).
    private let usageSession: URLSession = {
        let cfg = URLSessionConfiguration.ephemeral; cfg.timeoutIntervalForRequest = 15
        return URLSession(configuration: cfg)
    }()

    // Latest known state -- the source of truth we (re)send on heartbeat/reconnect.
    private var usage = Usage()          // merged provider array (from the mux); resent on heartbeat
    private var buddy = BuddyState()
    private var sessions: [Session] = []
    private var lastFix: Loc?   // most recent CoreLocation fix (issue #54); rides the (re)connect full frame
    private var heartbeat: Timer?

    // Per-provider usage reliability (#108). Any LIVE value (oauth poll or, for Claude, statusline
    // rate_limits) becomes last-known-good; a transient failure serves last-good as STALE. Keyed by id
    // so the reducer generalizes across providers. maxStale = 30 min.
    private let maxStale: TimeInterval = 1800
    private let inactiveThreshold: TimeInterval = 48 * 3600   // #126: demote an abandoned Claude to a quiet note
    private var descriptors: [String: ProviderDescriptor] = [:]
    private var registrationOrder: [String] = []
    private var retentions: [String: ProviderRetention] = [:]
    private var displays: [String: ProviderUsage] = [:]
    private var notes: [String: UsageNote] = [:]
    // Claude-only: its authoritative statusline source takes precedence over a late oauth poll (#93).
    private var statuslineClaude: ProviderUsage?   // #59 dedup
    private var statuslineClaudeAt: Date?          // #93 source-precedence gate
    private var claudeTransientReason: String?     // #108 backoff-window recheck reason
    // Per-provider hooks/setup state surfaced in the Settings window; AppDelegate owns the truth so a
    // toggle-driven refreshProviderToggles never clobbers a transient install spinner/note.
    private var providerHooks: [String: CheckState] = [:]
    private var providerInstalling: Set<String> = []
    private var providerNote: [String: String] = [:]
    private var checkBluetooth: CheckState = .checking
    private var checkPaired: CheckState = .checking

    func applicationDidFinishLaunching(_ notification: Notification) {
        startProviders()
        startCentral()
        startPoller()
        startSettings()
        startLoginItem()
        startLocation()
        startTickerEditor()

        // Heartbeat resends the full frame WITHOUT loc (issue #54): location rides the (re)connect frame
        // and on-change frames only, never the 30s heartbeat.
        heartbeat = Timer.scheduledTimer(withTimeInterval: 30, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.mux.reap(); self?.sendFullFrame(includeLocation: false) }
        }
        heartbeat?.tolerance = 3   // #66 L6: let the OS coalesce the 30s heartbeat wakeup.
    }

    func applicationDidBecomeActive(_ notification: Notification) {
        // Hooks can change out-of-band (manual edit) between launches; re-check on re-focus (off-main).
        refreshProviderHooks()
        refreshLoginItem()   // cheap re-sync; the menu-open refresh is the reliable path for this accessory app.
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        let drainers = [claude?.drainHeldPrompts, codex?.drainHeldPrompts].compactMap { $0 }
        guard !drainers.isEmpty else { return .terminateNow }
        var replied = false
        let reply = { if !replied { replied = true; NSApp.reply(toApplicationShouldTerminate: true) } }
        let group = DispatchGroup()
        for drain in drainers {
            group.enter()
            drain("Beacon hub is quitting", { group.leave() })
        }
        group.notify(queue: .main, execute: reply)
        // Safety cap so Quit never hangs if a socket write stalls; the drain replies earlier on real flush,
        // and immediately when nothing was held. A dropped conn would fail-OPEN per CONTRACT.md C.3.
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { reply() }
        return .terminateLater
    }

    // --- settings window + per-provider setup ---

    private func startSettings() {
        menubar.onOpenSettings = { [weak self] in self?.settingsWindow.show() }
        menubar.onInstallProviderHooks = { [weak self] id in self?.installHooks(for: id) }
        menubar.onOpenBluetooth = { SettingsLinks.open(SettingsLinks.bluetooth) }
        refreshProviderHooks()
        settingsWindow.showIfNeeded()
    }

    // Re-read every provider's hooks state off the main thread (sync file IO + parse), then apply on main
    // without stomping a provider mid-install.
    private func refreshProviderHooks() {
        let ids = providers.map { $0.descriptor.id }
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let states = ids.map { ($0, HooksInstaller.isInstalled(providerID: $0)) }
            Task { @MainActor in
                guard let self else { return }
                for (id, ok) in states where !self.providerInstalling.contains(id) {
                    self.providerHooks[id] = ok ? .ok : .bad
                }
                self.refreshProviderToggles()
                self.maybeMarkComplete()
            }
        }
    }

    // Install one provider's hooks (Process-backed, off the main thread), then re-check + surface a note.
    private func installHooks(for id: String) {
        guard !providerInstalling.contains(id) else { return }
        providerInstalling.insert(id)
        providerNote[id] = nil
        refreshProviderToggles()
        let label = descriptors[id]?.label.capitalized ?? id
        Task.detached { [weak self] in
            let errorMessage: String?
            do { try HooksInstaller.install(providerID: id); errorMessage = nil }
            catch { errorMessage = error.localizedDescription }
            let ok = HooksInstaller.isInstalled(providerID: id)
            guard let self else { return }
            await MainActor.run {
                self.providerInstalling.remove(id)
                self.providerHooks[id] = ok ? .ok : .bad
                self.providerNote[id] = ok ? "Installed. Restart \(label) for hooks to take effect." : errorMessage
                self.refreshProviderToggles()
                self.maybeMarkComplete()
            }
        }
    }

    // First-run auto-open stops once Bluetooth + pairing + every provider's hooks are satisfied.
    private func maybeMarkComplete() {
        let hooksOk = providers.allSatisfy { (providerHooks[$0.descriptor.id] ?? .checking) == .ok }
        if checkBluetooth == .ok, checkPaired == .ok, hooksOk { settingsWindow.markComplete() }
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
        // CoreBluetooth cannot clear the OS bond (no API); the app-side reset drops the link + rescans, so
        // a healthy bond reconnects on its own. The real "forget" is the user removing Beacon in Bluetooth
        // settings, so we jump straight there (the Settings window spells out the two clicks).
        SettingsLinks.open(SettingsLinks.bluetooth)
    }

    // --- providers + mux ---

    private func startProviders() {
        // Wire mux outputs FIRST so the initial register() emit (seeded usage / empty buddy) is captured.
        mux.onUsage = { [weak self] merged in
            guard let self else { return }
            self.usage = merged
            self.sendFrame(StatusFrame(usage: merged))
        }
        mux.onBuddy = { [weak self] state in self?.onBuddy(state) }
        mux.onSessions = { [weak self] sessions in
            guard let self else { return }
            self.sessions = sessions
            if let data = try? SessionsFrame(sessions).encoded() { self.central.send(data) }
        }
        mux.onAttention = { [weak self] in self?.menubar.playAttentionSoundIfEnabled() }
        mux.onPromptArrived = { [weak self] in self?.menubar.playPromptSoundIfEnabled() }
        mux.resolvePromptHandler = { [weak self] pid, nid, approve in
            guard let self, let p = self.providers.first(where: { $0.descriptor.id == pid })
            else { return .unknown }
            return p.resolvePrompt(nativeID: nid, approve: approve)
        }

        ingest.onStatus = { [weak self] msg in Task { @MainActor in self?.menubar.setBridgeAlert(msg) } }

        let claude = ClaudeCodeProvider(server: ingest, usageSession: usageSession)
        claude.onClaudeUsage = { [weak self] c in Task { @MainActor in self?.onStatuslineClaude(c) } }
        claude.onStatuslineActivity = { [weak self] in Task { @MainActor in self?.onStatuslineActivity() } }
        claude.onPromptUndeliverable = { [weak self] reason in
            Task { @MainActor in self?.menubar.setAlert("Auto-denied: \(reason)") }
        }
        self.claude = claude

        let codex = CodexProvider(server: ingest, usageSession: usageSession)
        self.codex = codex
        providers = [claude, codex]

        for p in providers {
            descriptors[p.descriptor.id] = p.descriptor
            registrationOrder.append(p.descriptor.id)
            let caps = settings.enabled(for: p.descriptor.id)
            mux.register(p.descriptor, enabled: caps)
            p.start(sink: mux)          // registers ingest routes before ingest.start()
            p.setEnabled(caps)          // apply a persisted-off buddy state on launch
        }
        ingest.start()

        // Provider toggle cards: seed + wire live setEnabled.
        menubar.onSetProviderUsage = { [weak self] id, on in self?.setProviderUsage(id, on) }
        menubar.onSetProviderBuddy = { [weak self] id, on in self?.setProviderBuddy(id, on) }
        refreshProviderToggles()
        pushMenubarUsage()
    }

    private func refreshProviderToggles() {
        let toggles = providers.map { p -> ProviderToggle in
            let id = p.descriptor.id
            let e = settings.enabled(for: id)
            return ProviderToggle(id: id, label: p.descriptor.label.capitalized,
                                  supportsUsage: p.descriptor.supportsUsage,
                                  supportsBuddy: p.descriptor.supportsBuddy,
                                  usageOn: e.usage, buddyOn: e.buddy,
                                  hooks: providerHooks[id] ?? .checking,
                                  installing: providerInstalling.contains(id),
                                  note: providerNote[id])
        }
        menubar.setProviderToggles(toggles)
    }

    private func setProviderUsage(_ id: String, _ on: Bool) { settings.setUsage(on, for: id); applyEnabled(id) }
    private func setProviderBuddy(_ id: String, _ on: Bool) { settings.setBuddy(on, for: id); applyEnabled(id) }

    private func applyEnabled(_ id: String) {
        let caps = settings.enabled(for: id)
        // Usage off: drop the retained note/retention/reason so a stale banner cannot resurface on
        // re-enable (#126). statuslineClaudeAt (liveness) and statuslineClaude (value cache, kept synced
        // by the ungated handler path) are intentionally preserved so re-enable is immediately correct.
        if !caps.usage {
            notes.removeValue(forKey: id)
            retentions.removeValue(forKey: id)
            if id == "claude" { claudeTransientReason = nil }
        }
        mux.setEnabled(id, caps)
        providers.first { $0.descriptor.id == id }?.setEnabled(caps)
        refreshProviderToggles()
        pushMenubarUsage()   // a usage toggle re-includes/excludes the provider's card immediately
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
                self?.reportAssembler.reset()   // discard any partial device report from a prior connection (#105)
                self?.sendFullFrame(includeLocation: true)
                if let data = try? SessionsFrame(self?.sessions ?? []).encoded() { self?.central.send(data) }
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
        claude?.setDeviceConnected(connected)
        codex?.setDeviceConnected(connected)
        poller.setDeviceConnected(connected)   // #64: back off the usage poll cadence while disconnected.

        // Drive the Settings connection checks from the SAME phase stream (no second CBCentralManager):
        // Bluetooth is bad only when powered-off/unauthorized/unavailable; paired tracks live .connected.
        switch phase {
        case .bluetoothOff, .unauthorized, .unavailable: checkBluetooth = .bad
        default:                                          checkBluetooth = .ok
        }
        checkPaired = connected ? .ok : .bad
        menubar.setBluetoothCheck(checkBluetooth)
        menubar.setPairedCheck(checkPaired)
        maybeMarkComplete()
    }

    private func handle(_ cmd: DeviceCommand) {
        switch cmd {
        case .permission(let id, let approve):
            // Ack the truth (issue #8): only ok:true when the decision actually applied. A late/
            // superseded decision => ok:false; an id we never minted => err.
            switch mux.resolve(shortId: id, approve: approve) {
            case .applied: central.send(HubAck.ack(id: id, ok: true))
            case .late:    central.send(HubAck.ack(id: id, ok: false))
            case .unknown: central.send(HubAck.err(id: id, reason: "unknown_prompt_id"))
            }
        case .configAck(let rev, let ok, let count, let err):
            // Ignore stale acks: a later edit already bumped our rev, so an ack for an older push no
            // longer reflects the desired state we're tracking (issue #92).
            guard rev == tickerStore.current.rev else { break }
            menubar.setTickerSync(ok ? .synced(count ?? tickerStore.current.rows.count)
                                     : .error(err ?? "rejected"))
        case .open(let id):
            // Route to the owning provider by short id; the provider focuses its own native session.
            guard let (pid, nativeKey) = mux.sessionRoute(shortId: id),
                  let p = providers.first(where: { $0.descriptor.id == pid }) else {
                FileHandle.standardError.write(Data("[beacon-hub] open id=\(id) -> unknown_session\n".utf8))
                central.send(HubAck.err(id: id, reason: "unknown_session"))
                return
            }
            DispatchQueue.global(qos: .userInitiated).async { [weak self] in
                let ok = p.focusSession(nativeKey: nativeKey)
                FileHandle.standardError.write(Data("[beacon-hub] open id=\(id) provider=\(pid) ok=\(ok)\n".utf8))
                DispatchQueue.main.async { self?.central.send(HubAck.ack(id: id, ok: ok)) }
            }
        case .report(_, _, _, _, _):
            switch reportAssembler.feed(cmd) {
            case .assembled(let rows): adoptDeviceReport(rows)
            case .pending, .dropped:   break
            }
        }
    }

    // --- poller ---

    private func startPoller() {
        poller = UsagePoller(providers: providers,
                             usageEnabled: { [weak self] id in self?.settings.enabled(for: id).usage ?? true })
        poller.onUpdate = { [weak self] results in Task { @MainActor in self?.onUsage(results) } }
        poller.setDeviceConnected(central.isConnected)
        poller.start()
    }

    // Per-provider poll results (#108). A provider absent from `results` was skipped this tick (gate /
    // disabled). Claude's authoritative statusline takes precedence over a late oauth poll, and when its
    // oauth poll is suppressed the retained value is still aged out so it can't pin stale past maxStale.
    private func onUsage(_ results: [String: ProviderResult]) {
        for id in registrationOrder where usageEnabled(id) {
            if id == "claude" {
                let age = statuslineClaudeAt.map { Date().timeIntervalSince($0) }
                let fresh = UsagePollDecision.statuslineFresh(age: age, interval: poller.pollInterval)
                if let r = results[id] {
                    if !fresh {
                        if case .transient(_, let reason) = r.outcome { claudeTransientReason = reason }
                        var outcome = r.outcome
                        if case .terminal(_, let kind) = outcome,
                           UsagePollDecision.providerInactive(kind: kind, statuslineAge: age,
                                                              threshold: inactiveThreshold) {
                            outcome = .inactive(reason: "Claude inactive")
                        }
                        reduce(id, outcome, r.usage)
                    }
                } else if !fresh, retentions[id]?.lastGood != nil {
                    reduce(id, .transient(retryAfter: nil,
                                          reason: claudeTransientReason ?? "Claude usage unavailable"), .unavailable)
                }
            } else if let r = results[id] {
                reduce(id, r.outcome, r.usage)
            }
        }
        pushMenubarUsage()
    }

    private func usageEnabled(_ id: String) -> Bool {
        (descriptors[id]?.supportsUsage ?? false) && settings.enabled(for: id).usage
    }

    // Liveness from Claude Code's statusline: fires on EVERY rate_limits POST (#93). A fresh POST means
    // the statusline is the live source, so re-affirm the cached value as LIVE -- clearing a stale
    // flag/note a prior oauth transient left even when the value callback was deduped (#59/#108).
    private func onStatuslineActivity() {
        statuslineClaudeAt = Date()
        guard usageEnabled("claude") else { return }
        guard let v = statuslineClaude else { return }
        var live = v; live.stale = nil
        if displays["claude"] != live {
            reduce("claude", .live, v); pushMenubarUsage()
        } else {
            retentions["claude"]?.lastGoodAt = Date()
        }
    }

    // Claude usage VALUE from the statusline rate_limits. Fed as a LIVE observation (becomes last-known-
    // good, survives a later 429). Deduped (#59).
    private func onStatuslineClaude(_ c: ProviderUsage) {
        guard c != statuslineClaude else { return }
        statuslineClaude = c
        guard usageEnabled("claude") else { return }
        reduce("claude", .live, c); pushMenubarUsage()
    }

    // Reduce one provider's observation into its retention/display/note, then feed the mux the display
    // value (the mux merges + dedups + emits the wire Usage; the BLE send rides mux.onUsage).
    private func reduce(_ id: String, _ outcome: ProviderOutcome, _ usage: ProviderUsage) {
        let prior = retentions[id] ?? ProviderRetention()
        let label = descriptors[id]?.label.capitalized ?? id
        let r = UsageReducer.reduceProvider(prior: prior, outcome: outcome, usage: usage,
                                            now: Date(), maxStale: maxStale, label: label)
        retentions[id] = r.next; displays[id] = r.display.usage
        if let note = r.display.note { notes[id] = note } else { notes.removeValue(forKey: id) }
        mux.provider(id, didUpdateUsage: r.display.usage)
    }

    // Push the merged usage + ordered notes to the menubar. The mux gates BLE frames on usage change;
    // menubar refresh is unconditional so a note-only change updates the UI without BLE traffic (#108).
    private func pushMenubarUsage() {
        let ordered = UsageReducer.visibleNotes(order: registrationOrder, notes: notes,
                                                enabled: usageEnabled)
        menubar.setUsage(usage, notes: ordered)
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

    // Adopt the device's reported ticker list on a fresh pairing (issue #105): only when our store is
    // pristine (rev 0, no rows) -- otherwise the hub stays the source of truth and its onReady push
    // reconciles the device. Persist (rev 0 -> 1) + refresh the panel; do NOT push back (the device
    // already has this list -- pushing would be a pointless echo).
    private func adoptDeviceReport(_ rows: [TickerRow]) {
        guard tickerStore.current.isPristine else { return }
        tickerStore.save(rows: rows)
        menubar.setTickerRows(tickerStore.current.rows)
        menubar.setTickerSync(.synced(tickerStore.current.rows.count))
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
            menubar.setTickerSync(.error("encode failed"))
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
