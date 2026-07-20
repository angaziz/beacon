import Foundation
import BeaconHubKit

// The Codex provider (design 2026-07-19). Usage comes from the existing ~/.codex/auth.json poller
// (CodexUsageProvider, unchanged). The buddy plane (sessions + prompts) rides Codex's command-type
// hooks, bridged through the beacon-codex-hook shim -> POST /codex/hook on the shared LocalIngestServer:
//   SessionStart      => register session (label = cwd basename + git branch)
//   UserPromptSubmit  => working
//   Stop              => attention
//   SessionEnd        => remove
//   PermissionRequest => held open until the device decides (mirrors ClaudeCodeProvider), fail-closed
//                        at a ~575s cap, strictly under the shim's 585s curl and Codex's 590s hook timeout.
// Byte-compatible with the Claude decision shape via HookResponse. State is confined to the ingest
// server's `queue`; sink calls hop to the main actor (where the mux lives). Logs only id + decision + ts.
final class CodexProvider: AgentProvider {
    static let hookPath = CodexHooks.routePath

    // Capabilities: usage (auth.json poll) + sessions + prompts (Codex hooks). The mux and per-provider
    // toggles already support the full tier.
    let descriptor = ProviderDescriptor(id: "codex", label: "CODEX",
                                        capabilities: [.usage, .sessions, .prompts])

    private let server: LocalIngestServer
    private let usageProvider: CodexUsageProvider
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

    // Branch resolution (git) runs off-queue; results hop back and feed the mux as a .branch event.
    var branchResolverForTest: ((String) -> String?)?
    private var branchCache: [String: String] = [:]
    private var branchInFlight: [String: [String]] = [:]
    private let gitQueue = DispatchQueue(label: "beacon.codex.git", qos: .utility)

    private static let isoStamp = ISO8601DateFormatter()

    init(server: LocalIngestServer, usageSession: URLSession = .shared) {
        self.server = server
        self.usageProvider = CodexUsageProvider(session: usageSession)
    }

    // --- AgentProvider ---

    func start(sink: ProviderSink) {
        self.sink = sink
        server.register(path: Self.hookPath) { [weak self] req in self?.handleHook(req) }
    }

    func setEnabled(_ caps: EnabledCapabilities) {
        queue.async { [weak self] in
            guard let self else { return }
            let wasBuddy = self.enabled.buddy
            self.enabled = caps
            // Buddy toggled OFF: release every held prompt pass-through (no verdict => Codex falls back
            // to its own TUI prompt); never auto-deny because a toggle is off (spec).
            if wasBuddy && !caps.buddy {
                for id in self.pending.filter({ !$0.value.done }).map(\.key) { self.releasePassthrough(id) }
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

    // Codex hooks carry no host-context (no beacon-session equivalent), so tap-to-open is unsupported.
    func focusSession(nativeKey: String) -> Bool { false }

    var usageSource: UsageProvider? { usageProvider }
    // No poll gate: Codex has no statusline-equivalent liveness source, so it always polls when enabled.

    // Mirror the BLE link state so an arriving prompt can be denied as "offline" instead of held
    // invisibly until the cap. Safe to call from any thread.
    func setDeviceConnected(_ connected: Bool) {
        queue.async { [weak self] in self?.deviceConnected = connected }
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
        switch event {
        case "PermissionRequest":
            handlePermission(req)
        case "SessionStart", "UserPromptSubmit", "Stop", "SessionEnd":
            applySessionHook(event: event, body: req.body)
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

    // Connection-agnostic permission logic (split out so tests drive it without a socket). Mirrors
    // ClaudeCodeProvider: fail-closed on quit/offline, pass-through on buddy-off, else hold for the
    // device. All responses use the Codex-compatible HookResponse shapes ({} = no verdict).
    private func permissionCore(body: [String: Any],
                                respond: @escaping (Data, (() -> Void)?) -> Void,
                                registerClose: (@escaping () -> Void) -> Void) {
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
        // Device offline => the prompt can't be shown. Deny immediately (named) rather than hold it
        // invisibly until the cap.
        if !deviceConnected {
            log(id: "-", decision: "auto-deny-offline")
            respond(HookResponse.permission(event: "PermissionRequest", allow: false, message: "Beacon device offline"), nil)
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
        // Fail-closed cap. STRICT ordering invariant: hub cap 575 < curl --max-time 585 < Codex hook
        // timeout 590. The hub MUST fire first so its deny reaches the still-open socket; if the cap
        // equaled the curl budget the socket would already be dead at the cap and Codex would degrade to
        // fail-open passthrough (curl's clock starts before the hub even receives the request).
        cap.schedule(deadline: .now() + 575)
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

    // Release a held prompt pass-through (buddy toggled off): no verdict, Codex falls back to its TUI.
    private func releasePassthrough(_ id: String) {
        guard let p = pending[id], !p.done else { return }
        p.done = true
        p.timeout.cancel()
        pending.removeValue(forKey: id)
        log(id: id, decision: "buddy-off-release-passthrough")
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

    private func applySessionHook(event: String, body: [String: Any]) {
        let sid = (body["session_id"] as? String) ?? ""
        guard !sid.isEmpty else { return }
        let cwd = body["cwd"] as? String
        switch event {
        case "Stop":
            emitSession(.stop(nativeKey: sid, cwd: cwd)); ensureBranch(sessionId: sid, cwd: cwd)
        case "SessionEnd":
            branchCache.removeValue(forKey: cwd ?? "")
            emitSession(.end(nativeKey: sid))
        default:   // SessionStart + UserPromptSubmit: activity establishes/keeps the session working.
            emitSession(.activity(nativeKey: sid, cwd: cwd)); ensureBranch(sessionId: sid, cwd: cwd)
        }
    }

    // Test seam: drive the session path without the Network/HTTP stack.
    func applySessionHookForTest(event: String, sessionId: String, cwd: String) {
        queue.sync { self.applySessionHook(event: event, body: ["session_id": sessionId, "cwd": cwd, "hook_event_name": event]) }
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
        FileHandle.standardError.write(Data("[beacon-hub] codex-perm id=\(id) decision=\(decision) at=\(ts)\n".utf8))
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
