import Foundation

// Pure per-session state machine (design §2/§3). Date-injected so it is unit-testable without wall time.
// The bridge feeds it from CC hooks; snapshot() renders the wire `[Session]`. Mints opaque monotonic
// `s<n>` ids (never reused in a process lifetime), distinct from the `p<n>` prompt namespace.
public final class SessionRegistry {
    private struct Entry {
        let shortId: String
        var cwd: String
        var branch: String?
        var updatedAt: Date     // sort key: max(last activity ts, stoppedAt)
        var stopped: Bool       // last lifecycle event was Stop, no newer activity => attention
        var needsInput: Bool = false   // CC Notification hook fired => session is waiting on user input
        // Host context captured by the beacon-session command hook on SessionStart (Phase 2a).
        var hostApp: String?    // TERM_PROGRAM (always set by CC env)
        var focusURL: String?   // WARP_FOCUS_URL — precise session handle (Warp only)
        var bundleId: String?   // __CFBundleIdentifier
    }
    private var entries: [String: Entry] = [:]    // keyed by CC session_id
    private var counter: UInt32 = 0
    private let idleTTL: TimeInterval
    private static let removeTTL: TimeInterval = 600

    public init(idleTTL: TimeInterval = 300) { self.idleTTL = idleTTL }

    // Opaque, monotonic, never reused for a live session. Wraps modulo 100000 to honor the frozen
    // 6-char id cap ("s" + <=5 digits); collisions are impossible in practice — reaching 100k sessions
    // in one hub process would require the first to be long gone (removeTTL = 600 s).
    private func mintId() -> String { counter = (counter &+ 1) % 100000; return "s\(counter)" }

    public func touchActivity(sessionId: String, cwd: String?, now: Date) {
        guard !sessionId.isEmpty else { return }
        if var e = entries[sessionId] {
            if let cwd, !cwd.isEmpty { e.cwd = cwd }
            e.updatedAt = now; e.stopped = false; e.needsInput = false
            entries[sessionId] = e
        } else {
            entries[sessionId] = Entry(shortId: mintId(), cwd: cwd ?? "", branch: nil,
                                       updatedAt: now, stopped: false)
        }
    }

    // Statusline liveness: a statusline render is a UI event, NOT proof the agent is working, so
    // unlike touchActivity this must NOT clear `stopped`/`needsInput`. Claude Code emits a final
    // statusline at/after a turn ends (to show the ending token/context count), firing ~3x/s while
    // active; if that trailing render cleared `stopped`, it would resurrect a just-Stopped session
    // back to .working and freeze it there once CC goes quiet (the "stuck on Working 2m" bug). We
    // still bump updatedAt so a long, permission-free turn doesn't cross idleTTL and wrongly go
    // .idle mid-work, and we still create the entry if statusline is the first signal we've seen.
    // The working transition now comes from real work signals only: UserPromptSubmit (per turn),
    // SessionStart, and permission requests -- all via touchActivity.
    public func touchStatusline(sessionId: String, cwd: String?, now: Date) {
        guard !sessionId.isEmpty else { return }
        if var e = entries[sessionId] {
            if let cwd, !cwd.isEmpty { e.cwd = cwd }
            e.updatedAt = now   // keep alive; preserve stopped/needsInput
            entries[sessionId] = e
        } else {
            entries[sessionId] = Entry(shortId: mintId(), cwd: cwd ?? "", branch: nil,
                                       updatedAt: now, stopped: false)
        }
    }

    // Mark a session as waiting on user input (CC Notification hook). Does NOT bump updatedAt so the
    // entry's sort position is preserved. Cleared by touchActivity (any new activity moves it on).
    public func markNeedsInput(sessionId: String) {
        guard !sessionId.isEmpty, var e = entries[sessionId] else { return }
        e.needsInput = true
        entries[sessionId] = e
    }

    public func markStop(sessionId: String, now: Date) {
        guard var e = entries[sessionId] else { return }
        e.stopped = true; e.updatedAt = now; entries[sessionId] = e
    }

    public func setBranch(sessionId: String, branch: String?) {
        guard var e = entries[sessionId] else { return }
        e.branch = (branch?.isEmpty == true) ? nil : branch; entries[sessionId] = e
    }

    // Merge host-context fields into an existing entry. No-op if the session isn't known yet
    // (in practice /session also calls touchActivity first, so it will always exist). Only a
    // non-empty incoming value overwrites; nil/"" leaves the prior value intact -- so a `compact`
    // event that doesn't re-export WARP_FOCUS_URL can't wipe a previously-captured precise handle.
    public func setHost(sessionId: String, hostApp: String?, focusURL: String?, bundleId: String?) {
        guard var e = entries[sessionId] else { return }
        if let v = hostApp, !v.isEmpty { e.hostApp = v }
        if let v = focusURL, !v.isEmpty { e.focusURL = v }
        if let v = bundleId, !v.isEmpty { e.bundleId = v }
        entries[sessionId] = e
    }

    // Look up host context by the full CC session_id. Returns nil for unknown sessions.
    public func host(for sessionId: String) -> (app: String?, focusURL: String?, bundleId: String?, cwd: String)? {
        guard let e = entries[sessionId] else { return nil }
        return (e.hostApp, e.focusURL, e.bundleId, e.cwd)
    }

    // Look up host context by the minted short id (s<n>) the device uses in open commands.
    // Returns nil when no entry matches.
    public func hostByShortId(_ id: String) -> (app: String?, focusURL: String?, bundleId: String?, cwd: String)? {
        guard let e = entries.values.first(where: { $0.shortId == id }) else { return nil }
        return (e.hostApp, e.focusURL, e.bundleId, e.cwd)
    }

    public func end(sessionId: String) { entries.removeValue(forKey: sessionId) }

    public func reap(now: Date) {
        let cutoff = now.addingTimeInterval(-Self.removeTTL)
        for (sid, e) in entries where e.updatedAt < cutoff { entries.removeValue(forKey: sid) }
    }

    public func snapshot(now: Date, waitingFront: String?, waitingQueued: Set<String>) -> [Session] {
        entries.map { (sid, e) -> (Date, Session) in
            let state: SessionState
            let stale = now.timeIntervalSince(e.updatedAt) >= idleTTL
            // Precedence: waiting (front) > waitingQueued > question > idle > attention > working.
            // question wins over stale/stopped: Claude is actively awaiting user input regardless of age.
            // waiting beats question: a held permission prompt is more urgent than a text question.
            if sid == waitingFront { state = .waiting }
            else if waitingQueued.contains(sid) { state = .waitingQueued }
            else if e.needsInput { state = .question }
            else if stale { state = .idle }                 // idle TTL wins over a long-ago Stop (Codex B3)
            else if e.stopped { state = .attention }
            else { state = .working }
            return (e.updatedAt, Session(id: e.shortId, label: Self.label(e.cwd, e.branch),
                                         state: state, ts: Int(e.updatedAt.timeIntervalSince1970)))
        }
        .sorted { $0.0 > $1.0 }
        .prefix(SessionLimits.maxCount)
        .map { $0.1 }
    }

    // Default branches add no information, so they are dropped ("redundant", spec §4). The device
    // splits the remaining "folder · branch" for its two-line row (Task 10).
    private static let redundantBranches: Set<String> = ["main", "master"]
    static func label(_ cwd: String, _ branch: String?) -> String {
        let folder = cwd.split(separator: "/").last.map(String.init) ?? cwd
        let showBranch = branch.map { !$0.isEmpty && !redundantBranches.contains($0) } ?? false
        let full = showBranch ? "\(folder) · \(branch!)" : folder
        return full.count <= SessionLimits.labelMaxChars
            ? full : String(full.prefix(SessionLimits.labelMaxChars))
    }
}
