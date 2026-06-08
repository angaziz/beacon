import Foundation
import Network
import BeaconHubKit

// Localhost HTTP/1.1 server that ingests Claude Code http hooks + the statusline shim, maps them to a
// BuddyState, and holds permission prompts open until the device decides (or a ~25 s fail-closed cap).
// Binds 127.0.0.1 on an ephemeral port written to ~/.beacon-hub/port (the hooks read it). Logs only
// id + decision + timestamp -- NEVER the hint/command string (tech.md 9).
final class ClaudeCodeBridge {
    var onBuddyUpdate: ((BuddyState) -> Void)?
    var onClaudeUsage: ((ProviderUsage) -> Void)?   // Claude 5h/7d from statusline rate_limits

    // Fixed localhost port so Claude Code's native http hooks use a static URL
    // (http://127.0.0.1:8765/hook). Also written to ~/.beacon-hub/port for the statusline shim.
    static let port: UInt16 = 8765

    private let listener: NWListener
    private let queue = DispatchQueue(label: "beacon.bridge")

    // Buddy idle fields accumulated from session/statusline hooks; the active prompt is overlaid.
    private var buddy = BuddyState()

    // Pending permission holds, keyed by the short BLE-safe id we mint. Resolving fulfills the held
    // HTTP response. Only ONE may be active (device holds a single prompt, records.h); a second hook
    // is auto-denied + labeled (chosen policy: never stack two long holds).
    private final class Pending {
        let respond: (Bool) -> Void   // true=allow, false=deny; idempotent (guarded by `done`).
        var done = false
        let timeout: DispatchSourceTimer
        init(respond: @escaping (Bool) -> Void, timeout: DispatchSourceTimer) {
            self.respond = respond; self.timeout = timeout
        }
    }
    private var pending: [String: Pending] = [:]
    private var activeId: String?
    private var idCounter: UInt32 = 0

    init() throws {
        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        params.requiredLocalEndpoint = NWEndpoint.hostPort(
            host: "127.0.0.1", port: NWEndpoint.Port(rawValue: Self.port)!)
        listener = try NWListener(using: params)   // binds 127.0.0.1:8765 (loopback only)
    }

    func start() {
        listener.newConnectionHandler = { [weak self] conn in self?.accept(conn) }
        listener.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            if case .ready = state, let p = self.listener.port?.rawValue {
                self.writePortFile(p)
            }
        }
        listener.start(queue: queue)
    }

    // resolve a held permission by the short id; approve=false denies. Safe to call from any thread.
    func resolve(id: String, approve: Bool) {
        queue.async { [weak self] in self?.finish(id: id, approve: approve, capped: false) }
    }

    // --- port file ---

    private func writePortFile(_ p: UInt16) {
        let dir = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".beacon-hub")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let file = dir.appendingPathComponent("port")
        try? "\(p)\n".data(using: .utf8)?.write(to: file)
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
            if let error = error { _ = error; conn.cancel(); return }
            var buf = buffer
            if let data = data { buf.append(data) }

            guard let headerEnd = Self.range(of: Data("\r\n\r\n".utf8), in: buf) else {
                if isComplete { conn.cancel(); return }
                self.readRequest(conn, buffer: buf)   // keep reading headers.
                return
            }
            let head = buf.subdata(in: buf.startIndex..<headerEnd.lowerBound)
            let bodyStart = headerEnd.upperBound
            let (method, path, contentLength) = Self.parseHead(head)
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
        case "SessionStart", "Stop", "Notification":
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
        let tool = (body["tool_name"] as? String) ?? (body["tool"] as? String) ?? "Tool"
        let hint = Self.commandHint(from: body["tool_input"]) ?? tool

        // One active prompt at a time: auto-deny a second concurrent permission (labeled).
        if activeId != nil {
            log(id: "-", decision: "auto-deny-busy")
            respondDecision(conn, allow: false, event: event)
            return
        }

        let shortId = mintId()
        let cappedTimer = DispatchSource.makeTimerSource(queue: queue)
        cappedTimer.schedule(deadline: .now() + 25)   // ~25 s fail-closed cap (D5, under CC's ~30 s).
        cappedTimer.setEventHandler { [weak self] in self?.finish(id: shortId, approve: false, capped: true) }

        let p = Pending(respond: { [weak self] allow in self?.respondDecision(conn, allow: allow, event: event) },
                        timeout: cappedTimer)
        pending[shortId] = p
        activeId = shortId
        cappedTimer.resume()

        buddy.prompt = BuddyPrompt(id: shortId, tool: tool, hint: hint)
        publishBuddy()
        log(id: shortId, decision: "prompt")
    }

    private func finish(id: String, approve: Bool, capped: Bool) {
        guard let p = pending[id], !p.done else { return }
        p.done = true
        p.timeout.cancel()
        pending[id] = nil
        if activeId == id { activeId = nil }
        if buddy.prompt?.id == id { buddy.prompt = nil; publishBuddy() }
        log(id: id, decision: capped ? "deny-timeout" : (approve ? "allow" : "deny"))
        p.respond(approve)
    }

    // --- session / statusline -> idle buddy fields ---

    private func applySessionHook(event: String, body: [String: Any]) {
        switch event {
        case "SessionStart":
            buddy.running += 1
        case "Stop":
            buddy.running = max(0, buddy.running - 1)
        case "Notification":
            // A Notification typically means CC is idle/waiting on the user.
            buddy.waiting += 1
        default: break
        }
        if let entry = Self.entryLine(event: event, body: body) {
            buddy.entries = (Array(buddy.entries.suffix(4)) + [entry]).suffix(5).map { $0 }
        }
        publishBuddy()
    }

    private func handleStatusline(_ body: [String: Any]) {
        // Confirmed Claude Code statusline schema: context_window.{used_percentage,total_input_tokens,
        // total_output_tokens}. (The payload also carries rate_limits.{five_hour,seven_day} -- a future
        // path to Claude usage without the Keychain/oauth call.)
        if let cw = body["context_window"] as? [String: Any] {
            if let pct = Self.double(cw["used_percentage"]) {
                buddy.contextPct = max(0, min(100, Int(pct.rounded())))
            }
            let inTok = Self.int(cw["total_input_tokens"]) ?? 0
            let outTok = Self.int(cw["total_output_tokens"]) ?? 0
            if inTok + outTok > 0 { buddy.tokens = inTok + outTok }
        }
        // Claude usage is also in the statusline (rate_limits) -- authoritative, no token/endpoint, so
        // it survives the oauth/usage 429. AppDelegate prefers this over the poller's Claude value.
        if let rl = body["rate_limits"] as? [String: Any] {
            let claude = ProviderUsage(h5: Self.rlWindow(rl["five_hour"]),
                                       d7: Self.rlWindow(rl["seven_day"]))
            let cb = onClaudeUsage
            DispatchQueue.main.async { cb?(claude) }
        }
        publishBuddy()
    }

    private static func rlWindow(_ any: Any?) -> UsageWindow {
        let w = any as? [String: Any]
        let pct = double(w?["used_percentage"]).map { max(0, min(100, Int($0.rounded()))) }
        let reset = int(w?["resets_at"]) ?? 0
        return UsageWindow(pct: pct, reset: reset)
    }

    private func publishBuddy() {
        let snapshot = buddy
        let cb = onBuddyUpdate
        DispatchQueue.main.async { cb?(snapshot) }
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
        let ts = ISO8601DateFormatter().string(from: Date())
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

    private static func entryLine(event: String, body: [String: Any]) -> String? {
        let f = DateFormatter(); f.dateFormat = "HH:mm"
        let stamp = f.string(from: Date())
        switch event {
        case "Stop": return "\(stamp) session stopped"
        case "Notification":
            if let msg = body["message"] as? String { return "\(stamp) \(msg.prefix(40))" }
            return "\(stamp) notification"
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

    private func respondDecision(_ conn: NWConnection, allow: Bool, event: String) {
        // Confirmed Claude Code permission-hook response: hookSpecificOutput.{hookEventName,
        // permissionDecision}. Echo the originating event so PermissionRequest is handled correctly.
        let payload: [String: Any] = [
            "hookSpecificOutput": [
                "hookEventName": event,
                "permissionDecision": allow ? "allow" : "deny",
                "permissionDecisionReason": allow ? "Approved on Beacon device" : "Denied on Beacon device",
            ],
        ]
        let data = (try? JSONSerialization.data(withJSONObject: payload)) ?? Data("{}".utf8)
        respond(conn, status: "200 OK", json: data)
    }

    private func respondJSON(_ conn: NWConnection, _ obj: [String: Any]) {
        let data = (try? JSONSerialization.data(withJSONObject: obj)) ?? Data("{}".utf8)
        respond(conn, status: "200 OK", json: data)
    }

    private func respond(_ conn: NWConnection, status: String, json: Data) {
        var head = "HTTP/1.1 \(status)\r\n"
        head += "Content-Type: application/json\r\n"
        head += "Content-Length: \(json.count)\r\n"
        head += "Connection: close\r\n\r\n"
        var out = Data(head.utf8)
        out.append(json)
        conn.send(content: out, completion: .contentProcessed { _ in conn.cancel() })
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
