import Foundation
import Network

// The shared localhost HTTP/1.1 ingest server (design 2026-07-19), generalized out of ClaudeCodeBridge.
// One NWListener on 127.0.0.1:8765 (fixed, so hooks use a static URL) with the resolved port written to
// ~/.beacon-hub/port. Providers register path-prefixed routes; Claude keeps /hook /statusline /session
// for shim back-compat, Codex will use /codex/hook. All route handlers run on `queue`, so a provider can
// use `queue` as its own state-confinement queue. Logs never carry hint/command strings (tech.md 9).
final class LocalIngestServer {
    // A matched POST. Handlers respond immediately (respondJSON/respondData) or capture `conn` to hold
    // the connection open (permission prompts) and respond later; watchClose fires on peer FIN/RST.
    struct Request {
        let conn: NWConnection
        let body: [String: Any]
        fileprivate let server: LocalIngestServer
        func respondJSON(_ obj: [String: Any]) { server.respondJSON(conn, obj) }
        func respondData(_ data: Data, onSent: (() -> Void)? = nil) {
            server.respond(conn, status: "200 OK", json: data, onSent: onSent)
        }
        func watchClose(_ handler: @escaping () -> Void) { server.watchForClose(conn, handler) }
    }

    static let port: UInt16 = 8765
    let queue = DispatchQueue(label: "beacon.ingest")
    var onStatus: ((String?) -> Void)?   // non-nil = bind failed (loud); nil = recovered

    private var routes: [String: (Request) -> Void] = [:]   // exact POST path -> handler
    private var listener: NWListener?
    private var listenerGen: UInt64 = 0
    private var rebindDelay: TimeInterval = 2
    private var waitingDebounce: DispatchWorkItem?

    // Register a route BEFORE start(). Path is matched exactly against the request line target.
    func register(path: String, handler: @escaping (Request) -> Void) { routes[path] = handler }

    func start() { queue.async { [weak self] in self?.bind() } }

    // --- listener lifecycle (ported verbatim from ClaudeCodeBridge) ---

    private static func makeListener() throws -> NWListener {
        let params = NWParameters.tcp
        // SO_REUSEADDR: only permits rebinding a port in TIME_WAIT (clean hub restart). It does NOT allow
        // two live listeners on 8765, so a real conflict still fails and bind-failure detection holds.
        params.allowLocalEndpointReuse = true
        params.requiredLocalEndpoint = NWEndpoint.hostPort(
            host: "127.0.0.1", port: NWEndpoint.Port(rawValue: Self.port)!)
        return try NWListener(using: params)
    }

    private func bind() {
        listenerGen &+= 1
        let gen = listenerGen
        let l: NWListener
        do { l = try Self.makeListener() } catch { reportFailure(gen: gen, error: error); return }
        l.newConnectionHandler = { [weak self] conn in self?.accept(conn) }
        l.stateUpdateHandler = { [weak self] state in self?.handleState(state, gen: gen) }
        listener = l
        l.start(queue: queue)
    }

    private func handleState(_ state: NWListener.State, gen: UInt64) {
        guard gen == listenerGen else { return }
        switch state {
        case .ready:
            if let p = listener?.port?.rawValue { writePortFile(p) }
            rebindDelay = 2
            waitingDebounce?.cancel(); waitingDebounce = nil
            let cb = onStatus
            DispatchQueue.main.async { cb?(nil) }
        case .failed(let error):
            reportFailure(gen: gen, error: error)
        case .waiting(let error):
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
        waitingDebounce?.cancel(); waitingDebounce = nil
        FileHandle.standardError.write(Data("[beacon-hub] ingest bind failed on port \(Self.port): \(error.localizedDescription)\n".utf8))
        let cb = onStatus
        DispatchQueue.main.async { cb?("Bridge offline - port \(Self.port) in use") }
        listener?.cancel(); listener = nil
        scheduleRebind()
    }

    private func scheduleRebind() {
        let delay = rebindDelay
        rebindDelay = min(rebindDelay * 2, 30)
        let gen = listenerGen
        queue.asyncAfter(deadline: .now() + delay) { [weak self] in
            guard let self, gen == self.listenerGen else { return }
            self.bind()
        }
    }

    private func writePortFile(_ p: UInt16) {
        let dir = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".beacon-hub")
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        } catch {
            FileHandle.standardError.write(Data("[beacon-hub] failed to create ~/.beacon-hub: \(error.localizedDescription)\n".utf8))
            return
        }
        let file = dir.appendingPathComponent("port")
        do { try "\(p)\n".data(using: .utf8)?.write(to: file) }
        catch { FileHandle.standardError.write(Data("[beacon-hub] failed to write port file: \(error.localizedDescription)\n".utf8)) }
    }

    // --- connection handling ---

    private func accept(_ conn: NWConnection) {
        conn.start(queue: queue)
        readRequest(conn, buffer: Data())
    }

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
                if isComplete || buf.count > 64 * 1024 { conn.cancel(); return }
                self.readRequest(conn, buffer: buf)
                return
            }
            let head = buf.subdata(in: buf.startIndex..<headerEnd.lowerBound)
            let bodyStart = headerEnd.upperBound
            let (method, path, contentLength) = Self.parseHead(head)
            guard contentLength >= 0, contentLength <= 1024 * 1024 else { conn.cancel(); return }
            let have = buf.count - bodyStart
            if have >= contentLength {
                let body = buf.subdata(in: bodyStart..<(bodyStart + contentLength))
                self.route(conn, method: method, path: path, body: body)
            } else if isComplete {
                conn.cancel()
            } else {
                self.readRequest(conn, buffer: buf)
            }
        }
    }

    private func route(_ conn: NWConnection, method: String, path: String, body: Data) {
        let obj = (try? JSONSerialization.jsonObject(with: body) as? [String: Any]) ?? [:]
        guard method == "POST", let handler = routes[path] else {
            respond(conn, status: "404 Not Found", json: Data("{}".utf8)); return
        }
        handler(Request(conn: conn, body: obj, server: self))
    }

    // Watch a parked connection for the peer closing it (the user answered on the Mac): the body is
    // already fully read, so this lone receive sees only EOF/error, never more data.
    private func watchForClose(_ conn: NWConnection, _ handler: @escaping () -> Void) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 1) { [weak self] _, _, isComplete, error in
            guard let self, isComplete || error != nil else { return }
            self.queue.async { handler() }
        }
    }

    // --- raw HTTP write ---

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
