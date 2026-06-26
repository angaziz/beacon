import Foundation
import Network
import BeaconHubKit

// Localhost HTTP/1.1 server that ingests Claude Code http hooks + the statusline shim, maps them to a
// BuddyState, and holds permission prompts open until the device decides (or a ~180 s fail-closed cap).
// Binds 127.0.0.1 on an ephemeral port written to ~/.beacon-hub/port (the hooks read it). Logs only
// id + decision + timestamp -- NEVER the hint/command string (tech.md 9).
final class ClaudeCodeBridge {
    // Outcome of resolving a device decision, so the caller can send a truthful ack (issue #8): the
    // decision either applied, arrived after the prompt already resolved (cap/superseded), or names an
    // id the hub never minted.
    enum ResolveOutcome {
        case applied
        case late      // id known but already done (e.g. lost the cap race) -> ack(ok:false)
        case unknown   // id never seen -> err(unknown_prompt_id)
    }

    var onBuddyUpdate: ((BuddyState) -> Void)?
    var onClaudeUsage: ((ProviderUsage) -> Void)?   // Claude 5h/7d from statusline rate_limits (deduped, #59)
    var onStatuslineActivity: (() -> Void)?         // liveness: fires on EVERY rate_limits POST, undeduped (#93)
    var onPromptUndeliverable: ((String) -> Void)?  // a prompt couldn't be shown (device offline)
    var onPromptArrived: (() -> Void)?              // a deliverable prompt was shown on the device (cue the user)
    var onAttention: (() -> Void)?                  // fires on aggregate 0->non-0 attention-bucket transition (spec §6)
    var onBridgeStatus: ((String?) -> Void)?        // non-nil = bind failed (loud); nil = recovered
    var onSessionsUpdate: (([Session]) -> Void)?

    // Fixed localhost port so Claude Code's native http hooks use a static URL
    // (http://127.0.0.1:8765/hook). Also written to ~/.beacon-hub/port for the statusline shim.
    static let port: UInt16 = 8765

    // A failed NWListener is terminal (cannot start() again), so recovery recreates it; hence a var.
    private var listener: NWListener?
    private let queue = DispatchQueue(label: "beacon.bridge")

    // Hoisted formatters (#66 L7): every caller runs on the serial `queue`, so one shared instance each
    // avoids constructing a formatter per log line / hook event.
    private static let isoStamp = ISO8601DateFormatter()
    private static let hm: DateFormatter = { let f = DateFormatter(); f.dateFormat = "HH:mm"; return f }()

    // Bumped on every bind(). State/timer closures capture their gen and early-return if it no longer
    // matches, so a delayed callback from a superseded listener can't clear/cancel/rebind a newer one.
    private var listenerGen: UInt64 = 0
    private var rebindDelay: TimeInterval = 2   // exponential backoff, reset to 2 on .ready.
    private var waitingDebounce: DispatchWorkItem?

    // Buddy idle fields accumulated from session/statusline hooks; the active prompt is overlaid.
    private var buddy = BuddyState()
    // Equality gates: CC re-invokes the statusline ~3x/s with byte-identical data (#59). Drop no-op
    // deltas -- the 30s heartbeat still resends the full frame, so a stalled consumer catches up.
    private var lastPublishedBuddy: BuddyState?
    private var lastClaudeUsage: ProviderUsage?
    private let registry = SessionRegistry(idleTTL: 300)
    private var lastPublishedSessions: [Session] = []

    // Injectable for tests; production runs `git -C <cwd> rev-parse --abbrev-ref HEAD` off-queue.
    var branchResolverForTest: ((String) -> String?)?
    private var branchCache: [String: String] = [:]            // cwd => resolved branch (queue-confined)
    private var branchInFlight: [String: [String]] = [:]       // cwd => sessionIds awaiting one in-flight resolve
    private let gitQueue = DispatchQueue(label: "beacon.git", qos: .utility)

    // Pending permission holds, keyed by the short BLE-safe id we mint. Resolving fulfills the held
    // HTTP response. Concurrent prompts queue (promptQueue, FIFO): the front is shown on the device and
    // deciding it advances to the next (issue #98).
    private final class Pending {
        // (allow, optional deny message, optional onSent fired once the deny bytes flush). Idempotent
        // (guarded by `done`). onSent is the seam the quit drain uses to wait for the socket write.
        let respond: (Bool, String?, (() -> Void)?) -> Void
        var done = false
        var resolvedAt: Date?   // set when done; lets the reaper bound the late-ack tombstone (see Task 3)
        let timeout: DispatchSourceTimer
        let sessionId: String         // the prompt's CC session_id, so finish() can decrement waitingSessionCounts.
        let tool: String
        let hint: String
        init(respond: @escaping (Bool, String?, (() -> Void)?) -> Void, timeout: DispatchSourceTimer,
             sessionId: String, tool: String, hint: String) {
            self.respond = respond; self.timeout = timeout; self.sessionId = sessionId
            self.tool = tool; self.hint = hint
        }
    }
    private var pending: [String: Pending] = [:]
    private var promptQueue: [String] = []   // FIFO of minted ids; promptQueue.first = the prompt shown on the device.
    private var idCounter: UInt32 = 0
    private var deviceConnected = false   // mirrored from BeaconCentral; mutated only on `queue`.
    private var terminating = false       // quit drain in progress (issue #16); queue-confined.

    // RUNNING is derived from a first-seen session map, NOT a SessionStart counter: SessionStart does
    // not support type:http in current CC (it may never fire through the bridge), and SessionEnd does
    // not fire on SIGKILL/crash. So we count any session we've SEEN recently (touch() on every event)
    // and reap stale ones on a TTL backstop -- robust to both missed-start and missed-end.
    private var sessions: [String: Date] = [:]            // session_id => lastSeen (queue-confined).
    private var waitingSessionCounts: [String: Int] = [:]   // sessionId => # of its prompts blocked on a user decision (queue-confined).
    private var sessionStats: [String: (tokens: Int, ctxPct: Int)] = [:]   // per-session statusline (queue-confined).
    private var reaper: DispatchSourceTimer?
    private static let sessionTTL: TimeInterval = 600      // 10 min: bounds a missed-SessionEnd leak.
    private static let tombstoneTTL: TimeInterval = 30   // keep a resolved id long enough for a racing late device decision -> .late

    init() {}

    func start() {
        queue.async { [weak self] in self?.bind(); self?.startReaper() }
    }

    // Repeating TTL reaper on `queue`: prunes sessions/waiting/stats whose session has gone silent for
    // > sessionTTL, then republishes if the derived counts changed. Bounds a SIGKILL/crash leak (no
    // SessionEnd) to <= one TTL instead of leaking forever. reap(now:) takes an explicit clock so it is
    // unit-testable without waiting on wall time.
    private func startReaper() {
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 60, repeating: 60, leeway: .seconds(5))   // #66 L6: coalesce wakeups.
        t.setEventHandler { [weak self] in self?.reap(now: Date()) }
        reaper = t
        t.resume()
    }

    private static func makeListener() throws -> NWListener {
        let params = NWParameters.tcp
        // SO_REUSEADDR: only permits rebinding a port in TIME_WAIT (clean hub restart). It does NOT
        // allow two live listeners on 8765 (that needs SO_REUSEPORT), so a real conflict still fails
        // and our bind-failure detection is unaffected -- keep this true, don't "fix" it.
        params.allowLocalEndpointReuse = true
        params.requiredLocalEndpoint = NWEndpoint.hostPort(
            host: "127.0.0.1", port: NWEndpoint.Port(rawValue: Self.port)!)
        return try NWListener(using: params)   // binds 127.0.0.1:8765 (loopback only)
    }

    // Recreate + start a listener. Runs on `queue`. A bind throw (port already held at launch) flows to
    // reportFailure just like an async .failed, closing the init-time silent-stderr gap.
    private func bind() {
        listenerGen &+= 1
        let gen = listenerGen
        let l: NWListener
        do {
            l = try Self.makeListener()
        } catch {
            reportFailure(gen: gen, error: error)
            return
        }
        l.newConnectionHandler = { [weak self] conn in self?.accept(conn) }
        l.stateUpdateHandler = { [weak self] state in self?.handleState(state, gen: gen) }
        listener = l
        l.start(queue: queue)
    }

    private func handleState(_ state: NWListener.State, gen: UInt64) {
        guard gen == listenerGen else { return }   // superseded listener; ignore.
        switch state {
        case .ready:
            if let p = listener?.port?.rawValue { writePortFile(p) }
            rebindDelay = 2
            waitingDebounce?.cancel(); waitingDebounce = nil
            let cb = onBridgeStatus
            DispatchQueue.main.async { cb?(nil) }
        case .failed(let error):
            reportFailure(gen: gen, error: error)
        case .waiting(let error):
            // .waiting is NOT terminal (transient flap at startup), so debounce ~1s before going loud;
            // if .ready arrives first it cancels this and we never flash a false alert.
            waitingDebounce?.cancel()
            let work = DispatchWorkItem { [weak self] in self?.reportFailure(gen: gen, error: error) }
            waitingDebounce = work
            queue.asyncAfter(deadline: .now() + 1, execute: work)
        default:
            break
        }
    }

    private func reportFailure(gen: UInt64, error: Error) {
        guard gen == listenerGen else { return }
        waitingDebounce?.cancel(); waitingDebounce = nil   // a real failure supersedes any pending .waiting debounce.
        FileHandle.standardError.write(Data("[beacon-hub] bridge bind failed on port \(Self.port): \(error.localizedDescription)\n".utf8))
        let cb = onBridgeStatus
        DispatchQueue.main.async { cb?("Bridge offline - port \(Self.port) in use") }
        listener?.cancel(); listener = nil
        scheduleRebind()
    }

    private func scheduleRebind() {
        let delay = rebindDelay
        rebindDelay = min(rebindDelay * 2, 30)
        let gen = listenerGen
        queue.asyncAfter(deadline: .now() + delay) { [weak self] in
            guard let self, gen == self.listenerGen else { return }   // newer cycle already ran.
            self.bind()
        }
    }

    // resolve a held permission by the short id; approve=false denies. Safe to call from any thread.
    // Runs finish synchronously on `queue` and returns its outcome so the caller can ack truthfully
    // (issue #8). Deadlock-safe on the real path: the BLE callback hops to @MainActor before calling
    // here, and finish only posts main callbacks / HTTP asynchronously (never blocks back on main).
    func resolve(id: String, approve: Bool) -> ResolveOutcome {
        queue.sync {
            // Device shows only the front; a decision for a live non-front id is out-of-order -> reject as
            // unknown (never advance the queue). A done id still falls through to finish -> .late tombstone.
            if let p = pending[id], !p.done, promptQueue.first != id { return .unknown }
            return finish(id: id, approve: approve, capped: false)
        }
    }

    // Look up host context by the device's short id (e.g. "s3"). Safe to call from any thread
    // (queue.sync). Returns nil when the session is unknown or has no host context.
    func hostForShortId(_ id: String) -> (app: String?, focusURL: String?, bundleId: String?, cwd: String)? {
        queue.sync { registry.hostByShortId(id) }
    }

    // Mirror the BLE link state so an arriving prompt can be denied as "offline" instead of held
    // invisibly until the cap. Safe to call from any thread.
    func setDeviceConnected(_ connected: Bool) {
        queue.async { [weak self] in self?.deviceConnected = connected }
    }

    // --- port file ---

    private func writePortFile(_ p: UInt16) {
        let dir = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".beacon-hub")
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        } catch {
            FileHandle.standardError.write(Data("[beacon-hub] failed to create ~/.beacon-hub: \(error.localizedDescription)\n".utf8))
            return
        }
        let file = dir.appendingPathComponent("port")
        do {
            try "\(p)\n".data(using: .utf8)?.write(to: file)
        } catch {
            FileHandle.standardError.write(Data("[beacon-hub] failed to write port file: \(error.localizedDescription)\n".utf8))
        }
    }

    // --- connection handling ---

    private func accept(_ conn: NWConnection) {
        conn.start(queue: queue)
        readRequest(conn, buffer: Data())
    }

    // Read until headers complete, then honor Content-Length for the body.
    private func readRequest(_ conn: NWConnection, buffer: Data) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            if let error = error {
                FileHandle.standardError.write(Data("[beacon-hub] connection read error: \(error.localizedDescription)\n".utf8))
                conn.cancel(); return
            }
            var buf = buffer
            if let data = data { buf.append(data) }

            guard let headerEnd = Self.range(of: Data("\r\n\r\n".utf8), in: buf) else {
                // #66 L9: a peer that never terminates the headers must not grow buf unbounded.
                if isComplete || buf.count > 64 * 1024 { conn.cancel(); return }
                self.readRequest(conn, buffer: buf)   // keep reading headers.
                return
            }
            let head = buf.subdata(in: buf.startIndex..<headerEnd.lowerBound)
            let bodyStart = headerEnd.upperBound
            let (method, path, contentLength) = Self.parseHead(head)
            // #66 L9: reject absurd or malformed (negative => inverted subdata range) Content-Length.
            guard contentLength >= 0, contentLength <= 1024 * 1024 else { conn.cancel(); return }
            let have = buf.count - bodyStart

            if have >= contentLength {
                let body = buf.subdata(in: bodyStart..<(bodyStart + contentLength))
                self.route(conn, method: method, path: path, body: body)
            } else if isComplete {
                conn.cancel()
            } else {
                self.readRequest(conn, buffer: buf)   // need more body.
            }
        }
    }

    private func route(_ conn: NWConnection, method: String, path: String, body: Data) {
        let obj = (try? JSONSerialization.jsonObject(with: body) as? [String: Any]) ?? [:]
        switch (method, path) {
        case ("POST", "/hook"):
            handleHook(conn, body: obj)
        case ("POST", "/statusline"):
            handleStatusline(obj)
            respondJSON(conn, ["ok": true])
        case ("POST", "/session"):
            handleSession(obj)
            respondJSON(conn, ["ok": true])
        default:
            respond(conn, status: "404 Not Found", json: Data("{}".utf8))
        }
    }

    // --- hook routing ---

    private func handleHook(_ conn: NWConnection, body: [String: Any]) {
        let event = (body["hook_event_name"] as? String) ?? ""
        switch event {
        case "PreToolUse", "PermissionRequest":
            handlePermission(conn, body: body)
        case "SessionStart", "Stop", "Notification", "SessionEnd":
            applySessionHook(event: event, body: body)
            respondJSON(conn, ["ok": true])
        default:
            respondJSON(conn, ["ok": true])
        }
    }

    private func handlePermission(_ conn: NWConnection, body: [String: Any]) {
        // Confirmed Claude Code PreToolUse/PermissionRequest fields: tool_name, tool_input{command|
        // file_path|description}, tool_use_id, session_id.
        let event = (body["hook_event_name"] as? String) ?? "PreToolUse"
        let sid = body["session_id"] as? String
        touch(sid)   // a permission request is liveness too -- keep the session in the running count.
        let tool = (body["tool_name"] as? String) ?? (body["tool"] as? String) ?? "Tool"
        // Front-load the cwd basename: with one prompt slot, the dir is the human disambiguator across
        // concurrent sessions (e.g. "[beacon] rm -rf build"). BUDDY_HINT_LEN=80 has room.
        let hint = Self.cwdTag(from: body) + (Self.commandHint(from: body["tool_input"]) ?? tool)

        // Quitting => deny any prompt landing in the drain window immediately (never held), so it can't
        // be dropped on exit (issue #16). Checked FIRST, before offline/busy.
        if terminating {
            log(id: "-", decision: "auto-deny-quit")
            respondDecision(conn, allow: false, event: event, message: "Beacon hub is quitting")
            return
        }

        // AskUserQuestion is a question, not a yes/no permission: the device can't pick an option, and a
        // held question would squat the single prompt slot (auto-denying real permissions that follow).
        // So never hold it -- defer to the Mac's interactive prompt ("ask") and just surface a passive
        // "asking a question" indicator. Before offline/busy: the human answers on the Mac regardless of
        // the device link, and a question must never consume the slot.
        if tool == "AskUserQuestion" {
            log(id: "-", decision: "question-passthrough")
            respondAsk(conn, event: event)
            if deviceConnected {
                buddy.entries = Array(([Self.questionEntry(from: body)] + buddy.entries).prefix(3))
                publishBuddy()
            }
            return
        }

        // Device offline => the prompt can't be shown. Deny immediately (named) rather than hold it
        // invisibly until the cap. Checked before busy: a disconnected device is undeliverable
        // regardless of any pending prompt. Respond first, THEN raise the alert, so the contract-
        // critical 2xx isn't gated on the callback's latency.
        if !deviceConnected {
            log(id: "-", decision: "auto-deny-offline")
            respondDecision(conn, allow: false, event: event, message: "Beacon device offline")
            onPromptUndeliverable?("Beacon device offline")
            return
        }

        // Concurrent prompts queue (FIFO) instead of auto-denying: the front is shown on the device,
        // deciding it advances to the next (issue #98).
        enqueuePrompt(respond: { [weak self] allow, msg, onSent in
                          self?.respondDecision(conn, allow: allow, event: event, message: msg, onSent: onSent)
                      },
                      sessionId: sid ?? "", tool: tool, hint: hint)
        watchForClose(conn, id: promptQueue.last!)   // peer closes => answered on the Mac => withdraw (issue #17)
    }

    // Test seam: run the held-prompt path without the Network/HTTP stack. respond is a no-op sink.
    func injectPermissionForTest(toolUseId: String, tool: String, hint: String) {
        queue.sync {
            self.enqueuePrompt(respond: { _, _, onSent in onSent?() },
                               sessionId: toolUseId, tool: tool, hint: hint)
        }
    }

    // Test seams: inspect the FIFO, and drive the hub-internal cap-expiry path (finish(capped:)) that
    // a prompt's 590s timer would fire -- NOT the device resolve path.
    func queuedIdsForTest() -> [String] { queue.sync { self.promptQueue } }
    func expirePromptForTest(id: String) { queue.sync { _ = self.finish(id: id, approve: false, capped: true) } }

    // Append a held prompt to the FIFO and publish the (possibly unchanged) front with the new depth.
    // Each prompt races its own 590s cap (under the 600s hook timeout); see finish/timeout handling.
    private func enqueuePrompt(respond: @escaping (Bool, String?, (() -> Void)?) -> Void,
                               sessionId: String, tool: String, hint: String) {
        if !sessionId.isEmpty {
            waitingSessionCounts[sessionId, default: 0] += 1
            buddy.waiting = waitingSessionCounts.count
            registry.touchActivity(sessionId: sessionId, cwd: nil, now: Date())
        }
        let shortId = mintId()
        let cap = DispatchSource.makeTimerSource(queue: queue)
        cap.schedule(deadline: .now() + 590)   // fail-closed, just under the 600s hook timeout (Task 3)
        cap.setEventHandler { [weak self] in self?.finish(id: shortId, approve: false, capped: true) }
        // Do NOT blanket-prune done tombstones here: with queuing, enqueues are frequent and would erase a
        // just-resolved id's tombstone, degrading a racing late device decision from .late to .unknown. The
        // reaper (Task 3) drops tombstones on a TTL instead.
        pending[shortId] = Pending(respond: respond, timeout: cap, sessionId: sessionId, tool: tool, hint: hint)
        promptQueue.append(shortId)
        cap.resume()
        publishFront()
        publishSessions()
        log(id: shortId, decision: "prompt")
        onPromptArrived?()
    }

    // A non-front (queued) prompt resolving -- including its own 590s cap firing -- removes just that id
    // and republishes the unchanged front with a lower qlen: the device never shows a "too late" for a
    // prompt it never displayed (design decision 5, issue #98). The front keeps today's on-screen UX.
    @discardableResult
    private func finish(id: String, approve: Bool, capped: Bool, message: String? = nil,
                        onSent: (() -> Void)? = nil) -> ResolveOutcome {
        guard let p = pending[id] else { return .unknown }
        guard !p.done else { return .late }   // already resolved (e.g. the cap fired first).
        p.done = true
        p.resolvedAt = Date()   // tombstone stamp; the reaper (Task 3) drops it after the late-ack TTL
        p.timeout.cancel()
        // Keep the resolved entry (done=true) so a later device decision for this same id reports
        // .late (=> ack ok:false) instead of .unknown (=> err) -- the core timeout-race case. The
        // reaper (Task 3) drops tombstones on a TTL. ids never repeat (mintId), so no stale-collision risk.
        promptQueue.removeAll { $0 == id }
        // The prompt is resolved => the session is one fewer blocked on the user.
        if !p.sessionId.isEmpty, let n = waitingSessionCounts[p.sessionId] {
            if n <= 1 { waitingSessionCounts.removeValue(forKey: p.sessionId) }
            else { waitingSessionCounts[p.sessionId] = n - 1 }
            buddy.waiting = waitingSessionCounts.count
        }
        publishFront()   // advances to the next front, or clears to idle when the FIFO is empty
        publishSessions()
        log(id: id, decision: capped ? "deny-timeout" : (approve ? "allow" : "deny"))
        p.respond(approve, message, onSent)
        return .applied
    }

    // Watch a parked permission connection for the peer (Claude Code) closing it. CC abandons the
    // in-flight hook -- closing the socket -- when the user answers the permission in the Mac terminal
    // instead of on the device (issue #17). The request body is already fully read, so this lone receive
    // sees only EOF/error (a client FIN/RST), never more data. Runs on `queue` (the connection's queue).
    private func watchForClose(_ conn: NWConnection, id: String) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 1) { [weak self] _, _, isComplete, error in
            guard let self, isComplete || error != nil else { return }
            self.queue.async { self.withdraw(id: id) }
        }
    }

    // Withdraw a held prompt that was resolved elsewhere (the Mac), as opposed to finish() which answers
    // CC. No HTTP write (the socket is already gone) and no deny -- just free the slot and clear the
    // device prompt SILENTLY, so a stale prompt can't self-expire to a false "too late - didn't apply"
    // and can't keep blocking later permissions. The !done guard makes our own finish()->cancel() (which
    // also fires watchForClose) a no-op: finish sets done before cancelling, so withdraw only wins when
    // the peer closes first.
    private func withdraw(id: String) {
        guard let p = pending[id], !p.done else { return }
        p.done = true
        p.resolvedAt = Date()   // tombstone stamp; the reaper (Task 3) drops it after the late-ack TTL
        p.timeout.cancel()
        promptQueue.removeAll { $0 == id }
        if !p.sessionId.isEmpty, let n = waitingSessionCounts[p.sessionId] {
            if n <= 1 { waitingSessionCounts.removeValue(forKey: p.sessionId) }
            else { waitingSessionCounts[p.sessionId] = n - 1 }
            buddy.waiting = waitingSessionCounts.count
        }
        publishFront()
        publishSessions()
        log(id: id, decision: "withdrawn-resolved-elsewhere")
    }

    // Quit drain (issue #16): deny every still-held prompt with a reason, and fire `completion` only once
    // the deny bytes have flushed to the socket (DispatchGroup over the per-send onSent), so CC gets a
    // clean answer instead of a dropped responder. Calls completion immediately when nothing is held.
    // Queue-confined.
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

    // --- session / statusline -> idle buddy fields ---

    private func applySessionHook(event: String, body: [String: Any]) {
        let sid = body["session_id"] as? String
        touch(sid)   // every session event is liveness; first-seen also covers SessionStart never firing.
        let cwd = body["cwd"] as? String
        switch event {
        case "Stop":
            // Turn finished (session still alive) => no longer blocked on the user; running unchanged.
            if let sid {
                waitingSessionCounts.removeValue(forKey: sid)
                registry.touchActivity(sessionId: sid, cwd: cwd, now: Date())
                registry.markStop(sessionId: sid, now: Date())
                ensureBranch(sessionId: sid, cwd: cwd)
            }
        case "Notification":
            // CC fires Notification when the session asks the user a question (not a permission prompt).
            // Mark needsInput so the device can surface a "tap to answer on Mac" takeover.
            if let sid { registry.markNeedsInput(sessionId: sid) }
        case "SessionEnd":
            // Clean exit (does NOT fire on SIGKILL -- the TTL reaper backstops that). Remove all trace.
            if let sid {
                sessions.removeValue(forKey: sid)
                waitingSessionCounts.removeValue(forKey: sid)
                sessionStats.removeValue(forKey: sid)
                registry.end(sessionId: sid)
            }
        default:   // SessionStart + any: activity establishes the session as working.
            if let sid { registry.touchActivity(sessionId: sid, cwd: cwd, now: Date()); ensureBranch(sessionId: sid, cwd: cwd) }
        }
        buddy.running = sessions.count
        buddy.waiting = waitingSessionCounts.count
        buddy.tokens = sessionStats.values.reduce(0) { $0 + $1.tokens }
        buddy.contextPct = sessionStats.values.map(\.ctxPct).max() ?? 0   // no reporters => 0, matching tokens (not stale).
        if let entry = Self.entryLine(event: event, body: body) {
            // Newest-first, cap 3 (= device BUDDY_ENTRIES): the device renders entries[0] as the
            // prominent slot, so the latest event must land at index 0 (the old suffix-append put it
            // last => device showed the oldest).
            buddy.entries = Array(([entry] + buddy.entries).prefix(3))
        }
        publishBuddy()
        publishSessions()
    }

    // internal (not private) so beacon-hubTests can drive the statusline path without the Network stack.
    func handleStatusline(_ body: [String: Any]) {
        // Confirmed Claude Code statusline schema: context_window.{used_percentage,total_input_tokens,
        // total_output_tokens}. (The payload also carries rate_limits.{five_hour,seven_day} -- a future
        // path to Claude usage without the Keychain/oauth call.)
        let sid = body["session_id"] as? String
        touch(sid)   // statusline traffic is the most frequent liveness signal.
        if let sid { registry.touchActivity(sessionId: sid, cwd: body["cwd"] as? String, now: Date()); ensureBranch(sessionId: sid, cwd: body["cwd"] as? String) }
        if let cw = body["context_window"] as? [String: Any] {
            let pct = Self.double(cw["used_percentage"]).map { max(0, min(100, Int($0.rounded()))) } ?? 0
            let inTok = Self.int(cw["total_input_tokens"]) ?? 0
            let outTok = Self.int(cw["total_output_tokens"]) ?? 0
            // Per-session aggregate (replaces last-writer-wins, which hid concurrent sessions). CC's
            // statusline carries session_id (CONTRACT.md C.4); if a live payload ever lacks it we fall
            // back to a single synthetic key (degrades to last-writer-wins, no invented wire field).
            sessionStats[sid ?? "_"] = (tokens: inTok + outTok, ctxPct: pct)
            buddy.tokens = sessionStats.values.reduce(0) { $0 + $1.tokens }   // total work in flight.
            buddy.contextPct = sessionStats.values.map(\.ctxPct).max() ?? 0   // most-pressured context.
        }
        // Claude usage is also in the statusline (rate_limits) -- authoritative, no token/endpoint, so
        // it survives the oauth/usage 429. AppDelegate prefers this over the poller's Claude value.
        if let rl = body["rate_limits"] as? [String: Any] {
            // #93: a rate_limits-carrying POST proves Claude Code is alive and feeding usage, regardless of
            // whether the parsed value changed. Fire liveness on EVERY such POST -- BEFORE the #59 value
            // dedup -- so the poller's statusline-freshness stamp can't age out during a flat session and
            // wrongly re-enable the (Keychain-prompting) Claude poll.
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
        publishBuddy()
        publishSessions()
    }

    // POST /session — receives host-context env from the beacon-session command hook on SessionStart.
    // Logs only host_app (never focus_url, which may embed a token per tech.md 9).
    private func handleSession(_ body: [String: Any]) {
        guard let sid = body["session_id"] as? String, !sid.isEmpty else { return }
        let cwd = body["cwd"] as? String
        let hostApp = body["host_app"] as? String
        let focusURL = body["focus_url"] as? String
        let bundleId = body["bundle_id"] as? String
        registry.touchActivity(sessionId: sid, cwd: cwd, now: Date())
        registry.setHost(sessionId: sid, hostApp: hostApp, focusURL: focusURL, bundleId: bundleId)
        if let app = hostApp, !app.isEmpty {
            FileHandle.standardError.write(Data("[beacon-hub] session host sid=\(sid.prefix(8)) app=\(app)\n".utf8))
        }
        publishSessions()
    }

    private static func rlWindow(_ any: Any?) -> UsageWindow {
        let w = any as? [String: Any]
        let pct = double(w?["used_percentage"]).map { max(0, min(100, Int($0.rounded()))) }
        let reset = int(w?["resets_at"]) ?? 0
        return UsageWindow(pct: pct, reset: reset)
    }

    // Record liveness for a session (queue-confined). First-seen establishes the running count even
    // when SessionStart never fires; re-touching on every event keeps it out of the TTL reaper.
    private func touch(_ sid: String?) {
        guard let sid, !sid.isEmpty else { return }
        sessions[sid] = Date()
    }

    // TTL backstop for sessions that died without a SessionEnd (SIGKILL/crash). Prunes silent sessions
    // and any waiting/stats they left behind, then republishes only if a derived count moved. Takes an
    // explicit `now` so it is unit-testable. internal so a future test can drive it directly.
    func reap(now: Date) {
        let cutoff = now.addingTimeInterval(-Self.sessionTTL)
        let before = (sessions.count, waitingSessionCounts.count, buddy.tokens, buddy.contextPct)
        for (sid, seen) in sessions where seen < cutoff { sessions.removeValue(forKey: sid) }
        waitingSessionCounts = waitingSessionCounts.filter { sessions[$0.key] != nil }   // a wait for a dead session is stuck.
        for sid in sessionStats.keys where sid != "_" && sessions[sid] == nil {
            sessionStats.removeValue(forKey: sid)   // a dead session must stop inflating the token sum.
        }
        let tombCutoff = now.addingTimeInterval(-Self.tombstoneTTL)
        for (pid, p) in pending where p.done && (p.resolvedAt ?? now) < tombCutoff {
            pending.removeValue(forKey: pid)   // bounded tombstone GC (replaces prune-on-enqueue)
        }
        buddy.running = sessions.count
        buddy.waiting = waitingSessionCounts.count
        buddy.tokens = sessionStats.values.reduce(0) { $0 + $1.tokens }
        buddy.contextPct = sessionStats.values.map(\.ctxPct).max() ?? 0   // no reporters => 0, matching tokens (not stale).
        if before != (sessions.count, waitingSessionCounts.count, buddy.tokens, buddy.contextPct) {
            publishBuddy()
        }
        publishSessions()
    }

    // Re-derive the device-visible prompt from the front of the FIFO + current depth, then publish.
    private func publishFront() {
        if let frontId = promptQueue.first, let p = pending[frontId], !p.done {
            let n = promptQueue.count
            buddy.prompt = BuddyPrompt(id: frontId, tool: p.tool, hint: p.hint, qlen: n > 1 ? n : nil)
        } else {
            buddy.prompt = nil
        }
        publishBuddy()
    }

    private func publishBuddy() {
        guard buddy != lastPublishedBuddy else { return }   // #59: central gate for all callers.
        lastPublishedBuddy = buddy
        let snapshot = buddy
        let cb = onBuddyUpdate
        DispatchQueue.main.async { cb?(snapshot) }
    }

    private func waitingSessionSets() -> (front: String?, queued: Set<String>) {
        guard let frontId = promptQueue.first, let fp = pending[frontId], !fp.done else { return (nil, []) }
        let front = fp.sessionId.isEmpty ? nil : fp.sessionId
        var queued = Set<String>()
        for id in promptQueue.dropFirst() {
            if let p = pending[id], !p.done, !p.sessionId.isEmpty { queued.insert(p.sessionId) }
        }
        queued.subtract(front.map { [$0] } ?? [])
        return (front, queued)
    }

    private func publishSessions() {
        registry.reap(now: Date())
        let (front, queued) = waitingSessionSets()
        let snap = registry.snapshot(now: Date(), waitingFront: front, waitingQueued: queued)
        guard snap != lastPublishedSessions else { return }
        let prevAttention = lastPublishedSessions.contains { $0.state == .attention }
        let nowAttention  = snap.contains { $0.state == .attention }
        if !prevAttention && nowAttention {     // bucket went 0 -> >0
            let cb = onAttention
            DispatchQueue.main.async { cb?() }
        }
        lastPublishedSessions = snap
        let cb = onSessionsUpdate
        DispatchQueue.main.async { cb?(snap) }
    }

    // Call from applySessionHook/handleStatusline AFTER registry.touchActivity, on `queue`. Dedupes by
    // cwd: a cached branch applies immediately; a cwd already resolving just appends the session to the
    // waiter list (Codex B4 -- never spawns a second git for the same cwd). git runs off `queue` (on
    // gitQueue) so a slow rev-parse never blocks a hook reply; the result hops back to `queue`.
    private func ensureBranch(sessionId: String, cwd: String?) {
        guard let cwd, !cwd.isEmpty else { return }
        if let cached = branchCache[cwd] { registry.setBranch(sessionId: sessionId, branch: cached); return }
        if branchInFlight[cwd] != nil { branchInFlight[cwd]!.append(sessionId); return }   // already resolving
        branchInFlight[cwd] = [sessionId]
        let resolver = branchResolverForTest
        gitQueue.async { [weak self] in
            let branch = resolver?(cwd) ?? Self.gitBranch(cwd)
            self?.queue.async {
                guard let self else { return }
                if let branch, !branch.isEmpty { self.branchCache[cwd] = branch }
                for sid in self.branchInFlight[cwd] ?? [] { self.registry.setBranch(sessionId: sid, branch: branch) }
                self.branchInFlight.removeValue(forKey: cwd)
                self.publishSessions()
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

    func applySessionHookForTest(event: String, sessionId: String, cwd: String) {
        queue.sync { self.applySessionHook(event: event, body: ["session_id": sessionId, "cwd": cwd, "hook_event_name": event]) }
    }

    // --- helpers ---

    private func mintId() -> String {
        idCounter &+= 1
        // <= 23 chars (records.h BUDDY_ID_LEN=24). "p" + counter + short uuid prefix => unique + short.
        let uuid = UUID().uuidString.prefix(8)
        return "p\(idCounter)-\(uuid)"   // e.g. "p3-1A2B3C4D" (<= 23 chars).
    }

    private func log(id: String, decision: String) {
        // Only id + decision + timestamp. NEVER the hint/command (tech.md 9).
        let ts = Self.isoStamp.string(from: Date())
        FileHandle.standardError.write(Data("[beacon-hub] perm id=\(id) decision=\(decision) at=\(ts)\n".utf8))
    }

    private static func commandHint(from input: Any?) -> String? {
        guard let dict = input as? [String: Any] else { return nil }
        // Bash commands carry the command; other tools fall back to a path/description.
        if let cmd = dict["command"] as? String { return cmd }
        if let path = dict["file_path"] as? String { return path }
        if let desc = dict["description"] as? String { return desc }
        return nil
    }

    // Trailing path component of body["cwd"], truncated for the device's BUDDY_ENTRY_LEN=40 budget.
    // Empty (no trailing tag) when cwd is absent, so callers can always concatenate. internal+static
    // so a future test can assert basename + truncation without the HTTP/Network stack.
    static func cwdTag(from body: [String: Any]) -> String {
        guard let cwd = body["cwd"] as? String, !cwd.isEmpty else { return "" }
        let base = (cwd as NSString).lastPathComponent
        return "[\(base.prefix(10))] "
    }

    // Passive device indicator for an AskUserQuestion (which is passed through, never held): a single
    // activity-feed line, same "HH:mm [dir] ..." shape as entryLine.
    static func questionEntry(from body: [String: Any]) -> String {
        return "\(hm.string(from: Date())) \(cwdTag(from: body))asking a question"
    }

    private static func entryLine(event: String, body: [String: Any]) -> String? {
        let stamp = hm.string(from: Date())
        let tag = cwdTag(from: body)   // e.g. "[beacon] "; empty when cwd absent.
        switch event {
        case "Stop": return "\(stamp) \(tag)turn done"           // turn finished, session still alive.
        case "SessionEnd": return "\(stamp) \(tag)session ended"
        case "Notification":
            if let msg = body["message"] as? String { return "\(stamp) \(tag)\(msg.prefix(40))" }
            return "\(stamp) \(tag)notification"
        default: return nil
        }
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

    // --- raw HTTP write ---

    private func respondDecision(_ conn: NWConnection, allow: Bool, event: String, message: String? = nil,
                                 onSent: (() -> Void)? = nil) {
        // PreToolUse and PermissionRequest need DIFFERENT decision shapes (HookResponse picks the right
        // one by event); the wrong shape silently fails to gate the tool. `message` names a deny cause.
        respond(conn, status: "200 OK", json: HookResponse.permission(event: event, allow: allow, message: message),
                onSent: onSent)
    }

    private func respondAsk(_ conn: NWConnection, event: String) {
        // Defer a question to Claude Code's own interactive prompt (no gate); see HookResponse.permissionAsk.
        respond(conn, status: "200 OK", json: HookResponse.permissionAsk(event: event))
    }

    private func respondJSON(_ conn: NWConnection, _ obj: [String: Any]) {
        let data = (try? JSONSerialization.data(withJSONObject: obj)) ?? Data("{}".utf8)
        respond(conn, status: "200 OK", json: data)
    }

    private func respond(_ conn: NWConnection, status: String, json: Data, onSent: (() -> Void)? = nil) {
        var head = "HTTP/1.1 \(status)\r\n"
        head += "Content-Type: application/json\r\n"
        head += "Content-Length: \(json.count)\r\n"
        head += "Connection: close\r\n\r\n"
        var out = Data(head.utf8)
        out.append(json)
        conn.send(content: out, completion: .contentProcessed { _ in conn.cancel(); onSent?() })
    }

    private static func parseHead(_ head: Data) -> (method: String, path: String, contentLength: Int) {
        guard let text = String(data: head, encoding: .utf8) else { return ("", "", 0) }
        let lines = text.components(separatedBy: "\r\n")
        var method = "", path = ""
        if let first = lines.first {
            let parts = first.split(separator: " ")
            if parts.count >= 2 { method = String(parts[0]); path = String(parts[1]) }
        }
        var length = 0
        for line in lines.dropFirst() where line.lowercased().hasPrefix("content-length:") {
            length = Int(line.dropFirst("content-length:".count).trimmingCharacters(in: .whitespaces)) ?? 0
        }
        return (method, path, length)
    }

    private static func range(of needle: Data, in haystack: Data) -> Range<Data.Index>? {
        guard !needle.isEmpty, haystack.count >= needle.count else { return nil }
        return haystack.range(of: needle)
    }
}
