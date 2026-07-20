import Foundation
import Network
import BeaconHubKit

// The Claude Code provider (design 2026-07-19), refactored from ClaudeCodeBridge. Ingests Claude Code
// http hooks + the statusline shim via the shared LocalIngestServer (routes /hook /statusline /session,
// byte-compatible for the installed shims), feeds the ProviderMux native session/prompt events, and
// holds permission prompts open until the device decides (or a ~590 s fail-closed cap). Capabilities:
// usage + sessions + prompts. All state is confined to the ingest server's `queue`; sink calls hop to
// the main actor (where the mux lives). Logs only id + decision + timestamp -- NEVER the hint/command.
final class ClaudeCodeProvider: AgentProvider {
    let descriptor = ProviderDescriptor(id: "claude", label: "CLAUDE",
                                        capabilities: [.usage, .sessions, .prompts])

    // Claude-specific usage callbacks the app wires to the reliability reducer (#59/#93/#108). The merged
    // usage the mux renders comes from the reducer via sink.didUpdateUsage, not from here directly.
    var onClaudeUsage: ((ProviderUsage) -> Void)?      // 5h/7d from statusline rate_limits (deduped, #59)
    var onStatuslineActivity: (() -> Void)?            // liveness: fires on EVERY rate_limits POST (#93)
    var onPromptUndeliverable: ((String) -> Void)?     // a prompt couldn't be shown (device offline)

    private let server: LocalIngestServer
    private weak var sink: ProviderSink?
    private var queue: DispatchQueue { server.queue }

    private let hosts = HostContextStore()
    private var enabled = EnabledCapabilities.all
    private var deviceConnected = false
    private var terminating = false

    // Per-session statusline aggregate (queue-confined) => BuddyState tokens/context_pct via metrics.
    private var sessionStats: [String: (tokens: Int, ctxPct: Int)] = [:]
    private var seen: [String: Date] = [:]             // session_id => lastSeen, for the stats TTL reaper.
    private var lastMetrics: (tokens: Int, ctxPct: Int) = (0, 0)
    private static let sessionTTL: TimeInterval = 600

    // Held permission prompts, keyed by a provider-native id we mint. Resolving fulfills the held HTTP
    // response. The device-facing short id + FIFO/qlen live in the mux's PromptBroker.
    private final class Pending {
        let event: String
        let respond: (Data, (() -> Void)?) -> Void   // writes an HTTP body to the held connection, closes.
        var done = false
        let timeout: DispatchSourceTimer
        let sessionKey: String   // CC session_id (may be empty)
        init(event: String, respond: @escaping (Data, (() -> Void)?) -> Void,
             timeout: DispatchSourceTimer, sessionKey: String) {
            self.event = event; self.respond = respond; self.timeout = timeout; self.sessionKey = sessionKey
        }
    }
    private var pending: [String: Pending] = [:]
    private var nativeCounter: UInt32 = 0

    private var lastClaudeUsage: ProviderUsage?

    // Branch resolution (git) runs off-queue; results hop back and feed the mux as a .branch event.
    var branchResolverForTest: ((String) -> String?)?
    private var branchCache: [String: String] = [:]
    private var branchInFlight: [String: [String]] = [:]
    private let gitQueue = DispatchQueue(label: "beacon.git", qos: .utility)

    // Usage poll gate (queue-independent; guarded by its own lock since the poller reads off-queue).
    private let gateLock = NSLock()
    private var lastStatuslineAt: Date?
    private var claudeFails = 0
    private var claudeBackoffUntil: Date?
    private let claudeBackoffCap: TimeInterval = 900
    private let retryAfterSanityCap: TimeInterval = 3600
    private let usageProvider: ClaudeUsageProvider

    private var reaper: DispatchSourceTimer?

    private static let isoStamp = ISO8601DateFormatter()
    private static let hm: DateFormatter = { let f = DateFormatter(); f.dateFormat = "HH:mm"; return f }()

    init(server: LocalIngestServer, usageSession: URLSession = .shared) {
        self.server = server
        self.usageProvider = ClaudeUsageProvider(session: usageSession)
    }

    // --- AgentProvider ---

    func start(sink: ProviderSink) {
        self.sink = sink
        server.register(path: "/hook") { [weak self] req in self?.handleHook(req) }
        server.register(path: "/statusline") { [weak self] req in
            self?.handleStatusline(req.body); req.respondJSON(["ok": true])
        }
        server.register(path: "/session") { [weak self] req in
            self?.handleSession(req.body); req.respondJSON(["ok": true])
        }
        queue.async { [weak self] in self?.startReaper() }
    }

    func setEnabled(_ caps: EnabledCapabilities) {
        queue.async { [weak self] in
            guard let self else { return }
            let wasBuddy = self.enabled.buddy
            self.enabled = caps
            // Buddy toggled OFF: release every held prompt pass-through (no verdict => the harness falls
            // back to its own interactive prompt), never auto-deny because a toggle is off (spec).
            if wasBuddy && !caps.buddy {
                for (id, _) in self.pending where !self.pending[id]!.done { self.releasePassthrough(id) }
            }
        }
    }

    func stop() { queue.async { [weak self] in self?.reaper?.cancel(); self?.reaper = nil } }

    func resolvePrompt(nativeID: String, approve: Bool) -> ResolveOutcome {
        queue.sync {
            guard let p = pending[nativeID] else { return .unknown }
            guard !p.done else { return .late }
            finish(id: nativeID, approve: approve, capped: false)
            return .applied
        }
    }

    func focusSession(nativeKey: String) -> Bool {
        let host = queue.sync { hosts.host(for: nativeKey) }
        guard let host else { return false }
        let target = FocusTarget(hostApp: host.app, focusURL: host.focusURL, bundleId: host.bundleId, cwd: host.cwd)
        return SessionFocus.focus(target)
    }

    var usageSource: UsageProvider? { usageProvider }

    func shouldPollUsage(now: Date, interval: TimeInterval) -> Bool {
        gateLock.lock(); defer { gateLock.unlock() }
        // While the statusline shim is fresh, skip the oauth (Keychain) call entirely (#93); also honor
        // the oauth backoff window so a 429 storm is not re-hit every tick (#108).
        let age = lastStatuslineAt.map { now.timeIntervalSince($0) }
        let backedOff = claudeBackoffUntil.map { now < $0 } ?? false
        return UsagePollDecision.shouldPollClaude(statuslineAge: age, interval: interval) && !backedOff
    }

    func noteUsageOutcome(_ outcome: ProviderOutcome) {
        gateLock.lock(); defer { gateLock.unlock() }
        switch outcome {
        case .live:
            claudeFails = 0; claudeBackoffUntil = nil
        case .transient(let retryAfter, _):
            claudeFails += 1
            let delay = UsagePollDecision.pollDelay(
                consecutiveFails: claudeFails, retryAfter: retryAfter,
                base: 45, cap: claudeBackoffCap, retryAfterSanityCap: retryAfterSanityCap,
                jitterFraction: Double.random(in: -0.2...0.2))
            claudeBackoffUntil = Date().addingTimeInterval(delay)
        case .terminal, .inactive:
            break
        }
    }

    // Mirror the BLE link state so an arriving prompt can be denied as "offline" instead of held
    // invisibly until the cap. Safe to call from any thread.
    func setDeviceConnected(_ connected: Bool) {
        queue.async { [weak self] in self?.deviceConnected = connected }
    }

    // --- reaper ---

    private func startReaper() {
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 60, repeating: 60, leeway: .seconds(5))
        t.setEventHandler { [weak self] in self?.reap(now: Date()) }
        reaper = t
        t.resume()
    }

    // Prune per-session stats for sessions gone silent past the TTL (SIGKILL/crash without SessionEnd),
    // then re-emit metrics if the totals moved. Session-list TTL is the mux registry's job.
    func reap(now: Date) {
        let cutoff = now.addingTimeInterval(-Self.sessionTTL)
        for (sid, at) in seen where at < cutoff {
            seen.removeValue(forKey: sid); sessionStats.removeValue(forKey: sid)
        }
        emitMetrics()
    }

    // --- hook routing ---

    private func handleHook(_ req: LocalIngestServer.Request) {
        let body = req.body
        let event = (body["hook_event_name"] as? String) ?? ""
        switch event {
        case "PreToolUse", "PermissionRequest":
            handlePermission(req)
        case "SessionStart", "Stop", "Notification", "SessionEnd":
            applySessionHook(event: event, body: body)
            req.respondJSON(["ok": true])
        default:
            req.respondJSON(["ok": true])
        }
    }

    private func handlePermission(_ req: LocalIngestServer.Request) {
        permissionCore(body: req.body,
                       respond: { data, onSent in req.respondData(data, onSent: onSent) },
                       registerClose: { onClose in req.watchClose(onClose) })
    }

    // Connection-agnostic permission logic. `respond` writes the hook decision to the held connection;
    // `registerClose` arms the withdraw-on-close watcher. Split out so tests drive it without a socket.
    private func permissionCore(body: [String: Any],
                                respond: @escaping (Data, (() -> Void)?) -> Void,
                                registerClose: (@escaping () -> Void) -> Void) {
        let event = (body["hook_event_name"] as? String) ?? "PreToolUse"
        let sid = (body["session_id"] as? String) ?? ""
        touch(sid)
        let tool = (body["tool_name"] as? String) ?? (body["tool"] as? String) ?? "Tool"
        let hint = Self.cwdTag(from: body) + (Self.commandHint(from: body["tool_input"]) ?? tool)

        // Quitting => deny any prompt landing in the drain window immediately (never held) (issue #16).
        if terminating {
            log(id: "-", decision: "auto-deny-quit")
            respond(HookResponse.permission(event: event, allow: false, message: "Beacon hub is quitting"), nil)
            return
        }

        // Buddy toggle OFF => pass-through immediately (no verdict); the harness prompts on the Mac (spec).
        if !enabled.buddy {
            log(id: "-", decision: "buddy-off-passthrough")
            respond(HookResponse.permissionAsk(event: event), nil)
            return
        }

        // AskUserQuestion is a question, not a yes/no permission: defer to the Mac's interactive prompt
        // and surface a passive "asking a question" indicator (never hold it).
        if tool == "AskUserQuestion" {
            log(id: "-", decision: "question-passthrough")
            respond(HookResponse.permissionAsk(event: event), nil)
            if deviceConnected { emitEntry(Self.questionEntry(from: body)) }
            return
        }

        // Device offline => the prompt can't be shown. Deny immediately (named) rather than hold it
        // invisibly until the cap. Respond first, THEN raise the alert.
        if !deviceConnected {
            log(id: "-", decision: "auto-deny-offline")
            respond(HookResponse.permission(event: event, allow: false, message: "Beacon device offline"), nil)
            let cb = onPromptUndeliverable
            DispatchQueue.main.async { cb?("Beacon device offline") }
            return
        }

        enqueuePrompt(event: event, respond: respond, sessionKey: sid, tool: tool, hint: hint,
                      registerClose: registerClose)
    }

    // Test seam: drive the full permission path (offline-deny / buddy-off / AskUserQuestion / hold)
    // without the Network/HTTP stack; `respond` captures the decision bytes.
    func handlePermissionForTest(body: [String: Any], respond: @escaping (Data, (() -> Void)?) -> Void) {
        queue.sync { self.permissionCore(body: body, respond: respond, registerClose: { _ in }) }
    }

    // Test seam: run the held-prompt path without the Network/HTTP stack. respond is a no-op sink.
    func injectPermissionForTest(sessionId: String, tool: String, hint: String) {
        queue.sync {
            self.enqueuePrompt(event: "PermissionRequest", respond: { _, onSent in onSent?() },
                               sessionKey: sessionId, tool: tool, hint: hint, registerClose: { _ in })
        }
    }

    private var lastNativeId: String?   // test convenience: most recent minted native id.
    func lastNativeIdForTest() -> String? { queue.sync { lastNativeId } }
    func heldCountForTest() -> Int { queue.sync { pending.values.filter { !$0.done }.count } }
    func expirePromptForTest(nativeID: String) { queue.sync { self.finish(id: nativeID, approve: false, capped: true) } }

    private func enqueuePrompt(event: String, respond: @escaping (Data, (() -> Void)?) -> Void,
                               sessionKey: String, tool: String, hint: String,
                               registerClose: (@escaping () -> Void) -> Void) {
        if !sessionKey.isEmpty { touch(sessionKey) }
        let nativeID = mintNativeId()
        lastNativeId = nativeID
        let cap = DispatchSource.makeTimerSource(queue: queue)
        cap.schedule(deadline: .now() + 590)   // fail-closed, just under the 600 s hook timeout.
        cap.setEventHandler { [weak self] in self?.finish(id: nativeID, approve: false, capped: true) }
        pending[nativeID] = Pending(event: event, respond: respond, timeout: cap, sessionKey: sessionKey)
        cap.resume()
        log(id: nativeID, decision: "prompt")
        // Raise to the broker (front/qlen) with the composite session key; the mux marks the session
        // .waiting and shows the front prompt on the device.
        emitRaise(nativeID: nativeID, tool: tool, hint: hint,
                  sessionNativeKey: sessionKey.isEmpty ? nil : sessionKey)
        // Peer closes the held connection => the user answered on the Mac; withdraw exactly THIS prompt.
        registerClose { [weak self] in self?.withdraw(id: nativeID) }
    }

    // Answer Claude Code (allow/deny/timeout) and free the slot; the broker drops it via didEndPrompt.
    private func finish(id: String, approve: Bool, capped: Bool, message: String? = nil,
                        onSent: (() -> Void)? = nil) {
        guard let p = pending[id], !p.done else { return }
        p.done = true
        p.timeout.cancel()
        pending.removeValue(forKey: id)
        log(id: id, decision: capped ? "deny-timeout" : (approve ? "allow" : "deny"))
        p.respond(HookResponse.permission(event: p.event, allow: approve, message: message), onSent)
        emitEndPrompt(id)
    }

    // Release a held prompt pass-through (buddy toggled off): no verdict, harness falls back to its UI.
    private func releasePassthrough(_ id: String) {
        guard let p = pending[id], !p.done else { return }
        p.done = true
        p.timeout.cancel()
        pending.removeValue(forKey: id)
        log(id: id, decision: "buddy-off-release-passthrough")
        p.respond(HookResponse.permissionAsk(event: p.event), nil)
        emitEndPrompt(id)
    }

    // Peer (Claude Code) closed the held connection => the user answered on the Mac. Free silently (no
    // HTTP write, no deny) so a stale prompt can't self-expire to a false "too late" and can't keep
    // blocking later permissions. The !done guard makes our own finish()->cancel() a no-op.
    private func withdraw(id: String) {
        guard let p = pending[id], !p.done else { return }
        p.done = true
        p.timeout.cancel()
        pending.removeValue(forKey: id)
        log(id: id, decision: "withdrawn-resolved-elsewhere")
        emitEndPrompt(id)
    }

    // Quit drain (issue #16): deny every still-held prompt, firing completion once the deny bytes flush.
    func drainHeldPrompts(reason: String, completion: @escaping () -> Void) {
        queue.async { [weak self] in
            guard let self else { DispatchQueue.main.async(execute: completion); return }
            self.terminating = true
            let heldIds = self.pending.filter { !$0.value.done }.map(\.key)
            guard !heldIds.isEmpty else { DispatchQueue.main.async(execute: completion); return }
            let group = DispatchGroup()
            for id in heldIds {
                group.enter()
                self.finish(id: id, approve: false, capped: false, message: reason, onSent: { group.leave() })
            }
            group.notify(queue: .main, execute: completion)
        }
    }

    // --- session / statusline ---

    private func applySessionHook(event: String, body: [String: Any]) {
        let sid = (body["session_id"] as? String) ?? ""
        touch(sid)
        let cwd = body["cwd"] as? String
        switch event {
        case "Stop":
            if !sid.isEmpty {
                emitSession(.stop(nativeKey: sid, cwd: cwd))
                ensureBranch(sessionId: sid, cwd: cwd)
            }
        case "Notification":
            if !sid.isEmpty { emitSession(.needsInput(nativeKey: sid, cwd: cwd)) }
        case "SessionEnd":
            if !sid.isEmpty {
                seen.removeValue(forKey: sid)
                sessionStats.removeValue(forKey: sid)
                hosts.remove(key: sid)
                emitSession(.end(nativeKey: sid))
            }
        default:   // SessionStart + any: activity establishes the session as working.
            if !sid.isEmpty { emitSession(.activity(nativeKey: sid, cwd: cwd)); ensureBranch(sessionId: sid, cwd: cwd) }
        }
        if let entry = Self.entryLine(event: event, body: body) { emitEntry(entry) }
        emitMetrics()
    }

    // internal so beacon-hubTests can drive the statusline path without the Network stack.
    func handleStatusline(_ body: [String: Any]) {
        let sid = (body["session_id"] as? String) ?? ""
        touch(sid)
        if !sid.isEmpty {
            emitSession(.activity(nativeKey: sid, cwd: body["cwd"] as? String))
            ensureBranch(sessionId: sid, cwd: body["cwd"] as? String)
        }
        if let cw = body["context_window"] as? [String: Any] {
            let pct = Self.double(cw["used_percentage"]).map { max(0, min(100, Int($0.rounded()))) } ?? 0
            let inTok = Self.int(cw["total_input_tokens"]) ?? 0
            let outTok = Self.int(cw["total_output_tokens"]) ?? 0
            sessionStats[sid.isEmpty ? "_" : sid] = (tokens: inTok + outTok, ctxPct: pct)
            emitMetrics()
        }
        // Claude usage is also in the statusline (rate_limits) -- authoritative, survives the oauth 429.
        if let rl = body["rate_limits"] as? [String: Any] {
            gateLock.lock(); lastStatuslineAt = Date(); gateLock.unlock()   // #93: keep the poll gate fresh.
            let cbActivity = onStatuslineActivity
            DispatchQueue.main.async { cbActivity?() }
            let claude = ProviderUsage(h5: Self.rlWindow(rl["five_hour"]),
                                       d7: Self.rlWindow(rl["seven_day"]))
            if claude != lastClaudeUsage {   // #59: skip the per-tick byte-identical resend of the value.
                lastClaudeUsage = claude
                let cb = onClaudeUsage
                DispatchQueue.main.async { cb?(claude) }
            }
        }
    }

    // POST /session -- host-context env from the beacon-session command hook on SessionStart.
    private func handleSession(_ body: [String: Any]) {
        guard let sid = body["session_id"] as? String, !sid.isEmpty else { return }
        let cwd = body["cwd"] as? String
        let hostApp = body["host_app"] as? String
        emitSession(.activity(nativeKey: sid, cwd: cwd))
        hosts.set(key: sid, app: hostApp, focusURL: body["focus_url"] as? String,
                  bundleId: body["bundle_id"] as? String, cwd: cwd)
        if let app = hostApp, !app.isEmpty {
            FileHandle.standardError.write(Data("[beacon-hub] session host sid=\(sid.prefix(8)) app=\(app)\n".utf8))
        }
    }

    // --- mux emission (all hop to main, where the mux lives) ---

    private func emitSession(_ event: ProviderSessionEvent) {
        let sink = sink; let id = descriptor.id
        DispatchQueue.main.async { sink?.provider(id, didUpdateSession: event) }
    }
    private func emitRaise(nativeID: String, tool: String, hint: String, sessionNativeKey: String?) {
        let sink = sink; let id = descriptor.id
        DispatchQueue.main.async {
            sink?.provider(id, didRaisePrompt: nativeID, tool: tool, hint: hint, sessionNativeKey: sessionNativeKey)
        }
    }
    private func emitEndPrompt(_ nativeID: String) {
        let sink = sink; let id = descriptor.id
        DispatchQueue.main.async { sink?.provider(id, didEndPrompt: nativeID) }
    }
    private func emitEntry(_ line: String) {
        let sink = sink; let id = descriptor.id
        DispatchQueue.main.async { sink?.provider(id, didAppendEntry: line) }
    }
    private func emitMetrics() {
        let tokens = sessionStats.values.reduce(0) { $0 + $1.tokens }
        let ctx = sessionStats.values.map(\.ctxPct).max() ?? 0
        guard (tokens, ctx) != lastMetrics else { return }
        lastMetrics = (tokens, ctx)
        let sink = sink; let id = descriptor.id
        DispatchQueue.main.async { sink?.provider(id, didUpdateMetrics: tokens, contextPct: ctx) }
    }

    // --- helpers ---

    private func touch(_ sid: String) { guard !sid.isEmpty else { return }; seen[sid] = Date() }

    private func mintNativeId() -> String { nativeCounter &+= 1; return "c\(nativeCounter)" }

    private func ensureBranch(sessionId: String, cwd: String?) {
        guard let cwd, !cwd.isEmpty else { return }
        if let cached = branchCache[cwd] { emitSession(.branch(nativeKey: sessionId, branch: cached)); return }
        if branchInFlight[cwd] != nil { branchInFlight[cwd]!.append(sessionId); return }
        branchInFlight[cwd] = [sessionId]
        let resolver = branchResolverForTest
        gitQueue.async { [weak self] in
            let branch = resolver?(cwd) ?? Self.gitBranch(cwd)
            self?.queue.async {
                guard let self else { return }
                if let branch, !branch.isEmpty { self.branchCache[cwd] = branch }
                for sid in self.branchInFlight[cwd] ?? [] { self.emitSession(.branch(nativeKey: sid, branch: branch)) }
                self.branchInFlight.removeValue(forKey: cwd)
            }
        }
    }

    func applySessionHookForTest(event: String, sessionId: String, cwd: String) {
        queue.sync { self.applySessionHook(event: event, body: ["session_id": sessionId, "cwd": cwd, "hook_event_name": event]) }
    }

    private static func gitBranch(_ cwd: String) -> String? {
        let p = Process(); p.executableURL = URL(fileURLWithPath: "/usr/bin/git")
        p.arguments = ["-C", cwd, "rev-parse", "--abbrev-ref", "HEAD"]
        let pipe = Pipe(); p.standardOutput = pipe; p.standardError = Pipe()
        do { try p.run() } catch { return nil }
        p.waitUntilExit()
        guard p.terminationStatus == 0 else { return nil }
        let out = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        let b = out.trimmingCharacters(in: .whitespacesAndNewlines)
        return (b.isEmpty || b == "HEAD") ? nil : b
    }

    private func log(id: String, decision: String) {
        let ts = Self.isoStamp.string(from: Date())
        FileHandle.standardError.write(Data("[beacon-hub] perm id=\(id) decision=\(decision) at=\(ts)\n".utf8))
    }

    private static func commandHint(from input: Any?) -> String? {
        guard let dict = input as? [String: Any] else { return nil }
        if let cmd = dict["command"] as? String { return cmd }
        if let path = dict["file_path"] as? String { return path }
        if let desc = dict["description"] as? String { return desc }
        return nil
    }

    static func cwdTag(from body: [String: Any]) -> String {
        guard let cwd = body["cwd"] as? String, !cwd.isEmpty else { return "" }
        let base = (cwd as NSString).lastPathComponent
        return "[\(base.prefix(10))] "
    }

    static func questionEntry(from body: [String: Any]) -> String {
        return "\(hm.string(from: Date())) \(cwdTag(from: body))asking a question"
    }

    private static func entryLine(event: String, body: [String: Any]) -> String? {
        let stamp = hm.string(from: Date())
        let tag = cwdTag(from: body)
        switch event {
        case "Stop": return "\(stamp) \(tag)turn done"
        case "SessionEnd": return "\(stamp) \(tag)session ended"
        case "Notification":
            if let msg = body["message"] as? String { return "\(stamp) \(tag)\(msg.prefix(40))" }
            return "\(stamp) \(tag)notification"
        default: return nil
        }
    }

    private static func rlWindow(_ any: Any?) -> UsageWindow {
        let w = any as? [String: Any]
        let pct = double(w?["used_percentage"]).map { max(0, min(100, Int($0.rounded()))) }
        let reset = int(w?["resets_at"]) ?? 0
        return UsageWindow(pct: pct, reset: reset)
    }

    private static func int(_ any: Any?) -> Int? {
        if let i = any as? Int { return i }
        if let d = any as? Double { return Int(d) }
        if let s = any as? String, let i = Int(s) { return i }
        return nil
    }
    private static func double(_ any: Any?) -> Double? {
        if let d = any as? Double { return d }
        if let i = any as? Int { return Double(i) }
        if let s = any as? String, let d = Double(s) { return d }
        return nil
    }
}
