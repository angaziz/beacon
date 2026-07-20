import Foundation

// Pure per-session state machine (design 2026-07-19). Date-injected so it is unit-testable without wall
// time. The ProviderMux feeds it native session events per provider; snapshot() renders the wire
// `[Session]`. Mints opaque monotonic `s<n>` short ids (never reused in a process lifetime), GLOBAL
// across providers, distinct from the `p<n>` prompt namespace. Each entry carries (providerID,
// nativeKey) so a device `open` short id routes back to the owning provider's native session, and the
// wire `agent` field names the provider.
public final class SessionRegistry {
    private struct Entry {
        let shortId: String
        let providerID: String      // "" for the default (single-provider) namespace used by kit tests
        let nativeKey: String       // the provider-native session id (e.g. a CC session_id)
        var cwd: String
        var branch: String?
        var updatedAt: Date         // sort key: max(last activity ts, stoppedAt)
        var stopped: Bool           // last lifecycle event was Stop, no newer activity => attention
        var needsInput: Bool = false   // a "needs input" signal fired => session is waiting on the user
    }
    private var entries: [String: Entry] = [:]    // keyed by composite key(providerID, nativeKey)
    private var counter: UInt32 = 0
    private let idleTTL: TimeInterval
    private static let removeTTL: TimeInterval = 600

    public init(idleTTL: TimeInterval = 300) { self.idleTTL = idleTTL }

    // Composite storage key. Empty providerID keeps the bare native key so existing single-provider
    // callers (and kit tests) address entries by plain session id.
    public static func key(providerID: String, nativeKey: String) -> String {
        providerID.isEmpty ? nativeKey : "\(providerID)\u{1}\(nativeKey)"
    }

    // Opaque, monotonic, never reused for a live session. Wraps modulo 100000 to honor the frozen
    // 6-char id cap ("s" + <=5 digits); collisions are impossible in practice -- reaching 100k sessions
    // in one hub process would require the first to be long gone (removeTTL = 600 s).
    private func mintId() -> String { counter = (counter &+ 1) % 100000; return "s\(counter)" }

    public func touchActivity(providerID: String = "", sessionId: String, cwd: String?, now: Date) {
        guard !sessionId.isEmpty else { return }
        let k = Self.key(providerID: providerID, nativeKey: sessionId)
        if var e = entries[k] {
            if let cwd, !cwd.isEmpty { e.cwd = cwd }
            e.updatedAt = now; e.stopped = false; e.needsInput = false
            entries[k] = e
        } else {
            entries[k] = Entry(shortId: mintId(), providerID: providerID, nativeKey: sessionId,
                               cwd: cwd ?? "", branch: nil, updatedAt: now, stopped: false)
        }
    }

    // Mark a session as waiting on user input (a "needs input" hook). Does NOT bump updatedAt so the
    // entry's sort position is preserved. Cleared by touchActivity (any new activity moves it on).
    public func markNeedsInput(providerID: String = "", sessionId: String) {
        let k = Self.key(providerID: providerID, nativeKey: sessionId)
        guard var e = entries[k] else { return }
        e.needsInput = true
        entries[k] = e
    }

    public func markStop(providerID: String = "", sessionId: String, now: Date) {
        let k = Self.key(providerID: providerID, nativeKey: sessionId)
        guard var e = entries[k] else { return }
        e.stopped = true; e.updatedAt = now; entries[k] = e
    }

    public func setBranch(providerID: String = "", sessionId: String, branch: String?) {
        let k = Self.key(providerID: providerID, nativeKey: sessionId)
        guard var e = entries[k] else { return }
        e.branch = (branch?.isEmpty == true) ? nil : branch; entries[k] = e
    }

    public func end(providerID: String = "", sessionId: String) {
        entries.removeValue(forKey: Self.key(providerID: providerID, nativeKey: sessionId))
    }

    // Resolve a device short id back to its owning provider + native session key (device `open`).
    public func route(shortId: String) -> (providerID: String, nativeKey: String)? {
        guard let e = entries.values.first(where: { $0.shortId == shortId }) else { return nil }
        return (e.providerID, e.nativeKey)
    }

    // Live-session count, optionally scoped to providers the caller still counts (buddy-enabled). Used
    // for BuddyState.running, which is uncapped (unlike the frame's 5-row snapshot).
    public func trackedCount(includeProvider: ((String) -> Bool)? = nil) -> Int {
        entries.values.reduce(0) { $0 + ((includeProvider?($1.providerID) ?? true) ? 1 : 0) }
    }

    public func reap(now: Date) {
        let cutoff = now.addingTimeInterval(-Self.removeTTL)
        for (k, e) in entries where e.updatedAt < cutoff { entries.removeValue(forKey: k) }
    }

    // `waitingFront`/`waitingQueued` are composite keys (key(providerID:nativeKey:)) so the mux's
    // broker sessions line up with the registry's entries. `includeProvider` drops buddy-disabled
    // providers BEFORE the cap so a disabled provider never squats a wire slot.
    public func snapshot(now: Date, waitingFront: String?, waitingQueued: Set<String>,
                         includeProvider: ((String) -> Bool)? = nil) -> [Session] {
        entries.compactMap { (k, e) -> (Date, Session)? in
            if let includeProvider, !includeProvider(e.providerID) { return nil }
            let state: SessionState
            let stale = now.timeIntervalSince(e.updatedAt) >= idleTTL
            // Precedence: waiting (front) > waitingQueued > question > idle > attention > working.
            // question wins over stale/stopped: the agent is actively awaiting user input regardless of
            // age. waiting beats question: a held permission prompt is more urgent than a text question.
            if k == waitingFront { state = .waiting }
            else if waitingQueued.contains(k) { state = .waitingQueued }
            else if e.needsInput { state = .question }
            else if stale { state = .idle }                 // idle TTL wins over a long-ago Stop
            else if e.stopped { state = .attention }
            else { state = .working }
            return (e.updatedAt, Session(id: e.shortId, label: Self.label(e.cwd, e.branch),
                                         state: state, ts: Int(e.updatedAt.timeIntervalSince1970),
                                         agent: e.providerID.isEmpty ? nil : e.providerID))
        }
        .sorted { $0.0 > $1.0 }
        .prefix(SessionLimits.maxCount)
        .map { $0.1 }
    }

    // Default branches add no information, so they are dropped ("redundant"). The device splits the
    // remaining "folder · branch" for its two-line row.
    private static let redundantBranches: Set<String> = ["main", "master"]
    static func label(_ cwd: String, _ branch: String?) -> String {
        let folder = cwd.split(separator: "/").last.map(String.init) ?? cwd
        let showBranch = branch.map { !$0.isEmpty && !redundantBranches.contains($0) } ?? false
        let full = showBranch ? "\(folder) · \(branch!)" : folder
        return full.count <= SessionLimits.labelMaxChars
            ? full : String(full.prefix(SessionLimits.labelMaxChars))
    }
}
