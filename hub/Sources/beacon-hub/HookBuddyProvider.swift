import Foundation
import BeaconHubKit

// Generic hook-buddy provider (issue #136; generalized from the 2026-07-19 CodexProvider). Drives the
// buddy plane (sessions + prompts) for any agent that POSTs the Claude/Codex-compatible hook shape to a
// LocalIngestServer route. Two agents use it today: Codex (via the beacon-codex-hook shim -> /codex/hook,
// with a ~/.codex/auth.json usage source) and omp (via the managed beacon.ts extension -> /omp/hook, no
// usage source). Session lifecycle:
//   SessionStart      => register session (label = cwd basename + git branch)
//   UserPromptSubmit  => working
//   Stop              => attention
//   SessionEnd        => remove
//   PermissionRequest => held open until the device decides (mirrors ClaudeCodeProvider), fail-closed at
//                        `capSeconds`. The cap MUST fire before the caller's own deadline so the deny
//                        reaches the still-open socket. Two timing chains, per instance:
//                          Codex: hub 575 < curl --max-time 585 < Codex hook timeout 590
//                          omp:   device 25 < hub 26 < extension fetch abort 28 < omp handler ceiling 30
// Byte-compatible with the Claude decision shape via HookResponse. State is confined to the ingest
// server's `queue`; sink calls hop to the main actor (where the mux lives). Logs only id + decision + ts.
final class HookBuddyProvider: AgentProvider {

    let descriptor: ProviderDescriptor
    let usageSource: UsageProvider?      // nil => no usage capability (omp); Codex passes its poller

    // A prompt couldn't be shown (device offline) => the app raises a menubar alert: the buddy is not
    // gating this agent until the link is back. Under omp's default `yolo` approvalMode a pass-through
    // means the tool simply runs, so this alert is the only signal the gate is absent.
    var onPromptUndeliverable: ((String) -> Void)?

    private let server: LocalIngestServer
    private let routePath: String        // ingest route this instance registers (e.g. /codex/hook)
    private let capSeconds: TimeInterval  // fail-closed hold cap; MUST fire before the caller's deadline
    private weak var sink: ProviderSink?
    private var queue: DispatchQueue { server.queue }

    private var enabled = EnabledCapabilities.all
    private var deviceConnected = false
    private var terminating = false

    // Held permission prompts, keyed by a provider-native id we mint. Resolving fulfills the held HTTP
    // response. The device-facing short id + FIFO/qlen live in the mux's PromptBroker.
    private final class Pending {
        let respond: (Data, (() -> Void)?) -> Void
        var done = false
        let timeout: DispatchSourceTimer
        init(respond: @escaping (Data, (() -> Void)?) -> Void, timeout: DispatchSourceTimer) {
            self.respond = respond; self.timeout = timeout
        }
    }
    private var pending: [String: Pending] = [:]
    private var nativeCounter: UInt32 = 0
    private var lastNativeId: String?

    // Tap-to-open host context (issue #136 follow-up). Populated from the hook body's host_app/
    // focus_url/bundle_id on SessionStart (the omp extension reads process.env; Codex sends none, so
    // its focus stays a no-op). Queue-confined like `pending`. focusRunner is injectable for tests.
    private let hosts = HostContextStore()
    private var focusRunner: (FocusTarget) -> Bool = { SessionFocus.focus($0) }

    // Branch resolution (git) runs off-queue; results hop back and feed the mux as a .branch event.
    var branchResolverForTest: ((String) -> String?)?
    private var branchCache: [String: String] = [:]
    private var branchInFlight: [String: [String]] = [:]
    private let gitQueue = DispatchQueue(label: "beacon.hookbuddy.git", qos: .utility)

    private static let isoStamp = ISO8601DateFormatter()

    init(descriptor: ProviderDescriptor, routePath: String, capSeconds: TimeInterval,
         server: LocalIngestServer, usageSource: UsageProvider? = nil) {
        self.descriptor = descriptor
        self.routePath = routePath
        self.capSeconds = capSeconds
        self.server = server
        self.usageSource = usageSource
    }

    // --- AgentProvider ---

    func start(sink: ProviderSink) {
        self.sink = sink
        server.register(path: routePath) { [weak self] req in self?.handleHook(req) }
    }

    func setEnabled(_ caps: EnabledCapabilities) {
        queue.async { [weak self] in
            guard let self else { return }
            let wasBuddy = self.enabled.buddy
            self.enabled = caps
            // Buddy toggled OFF: release every held prompt pass-through (no verdict => Codex falls back
            // to its own TUI prompt); never auto-deny because a toggle is off (spec).
            if wasBuddy && !caps.buddy {
                for id in self.pending.filter({ !$0.value.done }).map(\.key) {
                    self.releasePassthrough(id, reason: "buddy-off")
                }
            }
        }
    }

    func stop() {}

    func resolvePrompt(nativeID: String, approve: Bool) -> ResolveOutcome {
        queue.sync {
            guard let p = pending[nativeID] else { return .unknown }
            guard !p.done else { return .late }
            finish(id: nativeID, approve: approve, capped: false)
            return .applied
        }
    }

    // Tap-to-open: focus the terminal/editor that captured host context on SessionStart. Returns false
    // when no context was captured (Codex sends none => no-op, preserving its prior behavior). Called
    // off the main thread by AppDelegate (SessionFocus may briefly block on process launch).
    func focusSession(nativeKey: String) -> Bool {
        guard let host = queue.sync(execute: { hosts.host(for: nativeKey) }) else { return false }
        return focusRunner(FocusTarget(hostApp: host.app, focusURL: host.focusURL,
                                       bundleId: host.bundleId, cwd: host.cwd))
    }
    func setFocusRunnerForTest(_ f: @escaping (FocusTarget) -> Bool) { queue.sync { focusRunner = f } }

    // No poll gate: neither Codex nor omp has a statusline-equivalent liveness source, so they always
    // poll when enabled (omp has no usageSource at all).

    // Mirror the BLE link state: an arriving prompt passes through instead of being held invisibly, and
    // prompts already held when the link drops are released pass-through (they can no longer be
    // answered on the device). Safe to call from any thread.
    func setDeviceConnected(_ connected: Bool) {
        queue.async { [weak self] in
            guard let self else { return }
            let wasConnected = self.deviceConnected
            self.deviceConnected = connected
            if wasConnected && !connected {
                for id in self.pending.filter({ !$0.value.done }).map(\.key) {
                    self.releasePassthrough(id, reason: "offline")
                }
            }
        }
    }

    // Quit drain (mirrors ClaudeCodeProvider / issue #16): deny every still-held prompt (fail-closed),
    // firing completion once the deny bytes flush.
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

    // --- hook routing ---

    private func handleHook(_ req: LocalIngestServer.Request) {
        let event = (req.body["hook_event_name"] as? String) ?? ""
        if event == "PermissionRequest" { return handlePermission(req) }
        if let kind = SessionHookKind(hookEvent: event) { applySessionHook(kind, body: req.body) }
        req.respondJSON(["ok": true])
    }

    private func handlePermission(_ req: LocalIngestServer.Request) {
        permissionCore(body: req.body,
                       respond: { data, onSent in req.respondData(data, onSent: onSent) },
                       registerClose: { onClose in req.watchClose(onClose) })
    }

    // Connection-agnostic permission logic (split out so tests drive it without a socket). Mirrors
    // ClaudeCodeProvider: pass-through when this provider does not gate prompts, fail-closed on quit,
    // pass-through on buddy-off or an unreachable device, else hold for the device. All responses use
    // the Codex-compatible HookResponse shapes ({} = no verdict).
    private func permissionCore(body: [String: Any],
                                respond: @escaping (Data, (() -> Void)?) -> Void,
                                registerClose: (@escaping () -> Void) -> Void) {
        // Provider without the .prompts capability (omp since v4): it must never hold a tool call. A
        // stale v3 extension still POSTing PermissionRequest reads {} as passthrough, so it stops
        // gating immediately rather than at the next reinstall. Checked before `terminating`: a
        // non-gating provider has nothing to fail closed.
        if !descriptor.capabilities.contains(.prompts) {
            log(id: "-", decision: "no-prompt-capability-passthrough")
            respond(HookResponse.permissionAsk(event: "PermissionRequest"), nil)
            return
        }
        // Quitting => deny any prompt landing in the drain window immediately (never held).
        if terminating {
            log(id: "-", decision: "auto-deny-quit")
            respond(HookResponse.permission(event: "PermissionRequest", allow: false, message: "Beacon hub is quitting"), nil)
            return
        }
        // Buddy toggle OFF => pass-through immediately (no verdict); Codex prompts in its own TUI (spec).
        if !enabled.buddy {
            log(id: "-", decision: "buddy-off-passthrough")
            respond(HookResponse.permissionAsk(event: "PermissionRequest"), nil)
            return
        }
        // Device offline => the prompt can't be shown. Pass through (no verdict): "unreachable" is not a
        // decision, and a deny would fail a tool call the user never saw. Codex falls back to its own TUI
        // prompt; omp (built-in approval already done) just runs -- hence the menubar alert.
        if !deviceConnected {
            log(id: "-", decision: "offline-passthrough")
            respond(HookResponse.permissionAsk(event: "PermissionRequest"), nil)
            let cb = onPromptUndeliverable
            let label = descriptor.label
            DispatchQueue.main.async { cb?("Beacon offline - \(label) not gated") }
            return
        }
        enqueuePrompt(body: body, respond: respond, registerClose: registerClose)
    }

    private func enqueuePrompt(body: [String: Any],
                               respond: @escaping (Data, (() -> Void)?) -> Void,
                               registerClose: (@escaping () -> Void) -> Void) {
        let sid = (body["session_id"] as? String) ?? ""
        let tool = (body["tool_name"] as? String) ?? "Tool"
        let hint = Self.cwdTag(from: body) + (Self.commandHint(from: body["tool_input"]) ?? tool)
        let nativeID = mintNativeId()
        lastNativeId = nativeID
        let cap = DispatchSource.makeTimerSource(queue: queue)
        // Fail-closed cap. STRICT ordering invariant: the hub cap MUST fire before the caller's own
        // deadline so its deny reaches the still-open socket (Codex: 575 < curl 585 < hook 590; omp:
        // 26 < fetch abort 28 < handler ceiling 30). If the cap equaled the caller's budget the socket
        // would already be dead at the cap and the caller would degrade to fail-open passthrough.
        cap.schedule(deadline: .now() + capSeconds)
        cap.setEventHandler { [weak self] in self?.finish(id: nativeID, approve: false, capped: true) }
        pending[nativeID] = Pending(respond: respond, timeout: cap)
        cap.resume()
        log(id: nativeID, decision: "prompt")
        emitRaise(nativeID: nativeID, tool: tool, hint: hint, sessionNativeKey: sid.isEmpty ? nil : sid)
        // Peer closed the held connection => the user answered in the Codex TUI; withdraw THIS prompt.
        registerClose { [weak self] in self?.withdraw(id: nativeID) }
    }

    // Answer Codex (allow/deny/timeout) and free the slot; the broker drops it via didEndPrompt.
    private func finish(id: String, approve: Bool, capped: Bool, message: String? = nil,
                        onSent: (() -> Void)? = nil) {
        guard let p = pending[id], !p.done else { return }
        p.done = true
        p.timeout.cancel()
        pending.removeValue(forKey: id)
        log(id: id, decision: capped ? "deny-timeout" : (approve ? "allow" : "deny"))
        p.respond(HookResponse.permission(event: "PermissionRequest", allow: approve, message: message), onSent)
        emitEndPrompt(id)
    }

    // Release a held prompt pass-through (buddy toggled off, or the link dropped): no verdict, the agent
    // falls back to its own TUI prompt. `reason` only labels the log line.
    private func releasePassthrough(_ id: String, reason: String) {
        guard let p = pending[id], !p.done else { return }
        p.done = true
        p.timeout.cancel()
        pending.removeValue(forKey: id)
        log(id: id, decision: "\(reason)-release-passthrough")
        p.respond(HookResponse.permissionAsk(event: "PermissionRequest"), nil)
        emitEndPrompt(id)
    }

    // Peer (Codex) closed the held connection => the user answered in the TUI. Free silently (no HTTP
    // write, no deny) so a stale prompt can't self-expire to a false verdict or block later permissions.
    private func withdraw(id: String) {
        guard let p = pending[id], !p.done else { return }
        p.done = true
        p.timeout.cancel()
        pending.removeValue(forKey: id)
        log(id: id, decision: "withdrawn-resolved-elsewhere")
        emitEndPrompt(id)
    }

    // --- session lifecycle ---

    // Hook event -> registry transition. This init IS the routed-event allowlist: handleHook forwards
    // exactly the events it recognizes, so a new event can never half-land (routed but unmapped, or
    // mapped but unrouted). Unknown events are acknowledged and ignored.
    enum SessionHookKind {
        case activity, stop, needsInput, end
        init?(hookEvent: String) {
            switch hookEvent {
            case "SessionStart", "UserPromptSubmit": self = .activity
            case "ApprovalResolved":                 self = .activity   // omp: approval answered => working
            case "Notification":                     self = .needsInput // omp: prompt on screen on the Mac
            case "Stop":                             self = .stop
            case "SessionEnd":                       self = .end
            default:                                 return nil
            }
        }
    }

    private func applySessionHook(_ kind: SessionHookKind, body: [String: Any]) {
        let sid = (body["session_id"] as? String) ?? ""
        guard !sid.isEmpty else { return }
        let cwd = body["cwd"] as? String
        switch kind {
        case .stop:
            emitSession(.stop(nativeKey: sid, cwd: cwd)); ensureBranch(sessionId: sid, cwd: cwd)
        case .end:
            branchCache.removeValue(forKey: cwd ?? "")
            hosts.remove(key: sid)
            emitSession(.end(nativeKey: sid))
        case .needsInput:
            emitSession(.needsInput(nativeKey: sid, cwd: cwd)); ensureBranch(sessionId: sid, cwd: cwd)
        case .activity:
            // SessionStart carries tap-to-open host context (omp reads process.env; merge keeps prior
            // non-empty values so a later UserPromptSubmit without env can't wipe them).
            hosts.set(key: sid, app: body["host_app"] as? String, focusURL: body["focus_url"] as? String,
                      bundleId: body["bundle_id"] as? String, cwd: cwd)
            emitSession(.activity(nativeKey: sid, cwd: cwd)); ensureBranch(sessionId: sid, cwd: cwd)
        }
    }

    // Test seam: drive the session path without the Network/HTTP stack. Takes the wire event name and
    // resolves it through SessionHookKind, so a test also exercises the routed-event allowlist; an
    // unroutable event returns false rather than silently doing nothing.
    @discardableResult
    func applySessionHookForTest(event: String, sessionId: String, cwd: String,
                                 hostApp: String? = nil, focusURL: String? = nil,
                                 bundleId: String? = nil) -> Bool {
        guard let kind = SessionHookKind(hookEvent: event) else { return false }
        queue.sync {
            var body: [String: Any] = ["session_id": sessionId, "cwd": cwd, "hook_event_name": event]
            if let hostApp { body["host_app"] = hostApp }
            if let focusURL { body["focus_url"] = focusURL }
            if let bundleId { body["bundle_id"] = bundleId }
            self.applySessionHook(kind, body: body)
        }
        return true
    }

    // Test seams for the permission path (mirrors ClaudeCodeProvider's).
    func handlePermissionForTest(body: [String: Any], respond: @escaping (Data, (() -> Void)?) -> Void) {
        queue.sync { self.permissionCore(body: body, respond: respond, registerClose: { _ in }) }
    }
    func injectPermissionForTest(sessionId: String, tool: String, hint: String) {
        queue.sync {
            self.enqueuePrompt(body: ["session_id": sessionId, "tool_name": tool,
                                      "tool_input": ["command": hint], "hook_event_name": "PermissionRequest"],
                               respond: { _, onSent in onSent?() }, registerClose: { _ in })
        }
    }
    func lastNativeIdForTest() -> String? { queue.sync { lastNativeId } }
    func heldCountForTest() -> Int { queue.sync { pending.values.filter { !$0.done }.count } }
    func expirePromptForTest(nativeID: String) { queue.sync { self.finish(id: nativeID, approve: false, capped: true) } }

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

    // --- helpers ---

    private func mintNativeId() -> String { nativeCounter &+= 1; return "x\(nativeCounter)" }

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
        FileHandle.standardError.write(Data("[beacon-hub] \(descriptor.id)-perm id=\(id) decision=\(decision) at=\(ts)\n".utf8))
    }

    private static func commandHint(from input: Any?) -> String? {
        guard let dict = input as? [String: Any] else { return nil }
        if let cmd = dict["command"] as? String { return cmd }
        if let path = dict["file_path"] as? String { return path }
        if let path = dict["path"] as? String { return path }
        return nil
    }

    static func cwdTag(from body: [String: Any]) -> String {
        guard let cwd = body["cwd"] as? String, !cwd.isEmpty else { return "" }
        let base = (cwd as NSString).lastPathComponent
        return "[\(base.prefix(10))] "
    }
}
