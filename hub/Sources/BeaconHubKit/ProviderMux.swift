import Foundation

// A native session lifecycle event a provider feeds to the mux (before global short-id minting). The
// mux stamps its own clock, so events carry no timestamp.
public enum ProviderSessionEvent: Equatable {
    case activity(nativeKey: String, cwd: String?)      // any liveness => working, clears attention/question
    case stop(nativeKey: String, cwd: String?)          // turn finished (session alive) => attention
    case needsInput(nativeKey: String, cwd: String?)    // asking the user a question => question
    case branch(nativeKey: String, branch: String?)     // resolved git branch for the label
    case end(nativeKey: String)                          // clean session exit => remove
}

// Mux-facing event callbacks a started provider drives. The mux implements this; providers hold a
// reference handed to start(sink:). Providers MUST call these on a single consistent queue (the app
// hops to the main actor), since the mux is not internally synchronized.
public protocol ProviderSink: AnyObject {
    func provider(_ id: String, didUpdateUsage usage: ProviderUsage)
    func provider(_ id: String, didUpdateMetrics tokens: Int, contextPct: Int)
    func provider(_ id: String, didUpdateSession event: ProviderSessionEvent)
    func provider(_ id: String, didRaisePrompt nativeID: String, tool: String, hint: String,
                  sessionNativeKey: String?)
    func provider(_ id: String, didEndPrompt nativeID: String)
    func provider(_ id: String, didAppendEntry line: String)
}

// The pure cross-provider aggregator (design 2026-07-19). Owns global short-id minting (SessionRegistry
// for sessions, PromptBroker for prompts), applies the per-provider Usage/Buddy toggles, and emits the
// merged wire state: Usage (enabled usage providers, registration order), newest-first [Session], and a
// BuddyState counting across buddy-enabled providers. Routes device permission/open commands back to
// the owning provider via the injected handlers. No Foundation networking -- host-tested.
public final class ProviderMux: ProviderSink {
    // Outputs (dedup-gated). The app caches the latest and (re)sends on connect/heartbeat.
    public var onUsage: ((Usage) -> Void)?
    public var onBuddy: ((BuddyState) -> Void)?
    public var onSessions: (([Session]) -> Void)?
    public var onAttention: (() -> Void)?   // aggregate 0 -> >0 attention-bucket transition

    // Device permission decisions dispatch into the concrete provider (app wires this; kit stays
    // networking-free). `open` uses sessionRoute below -- focus itself runs async in the app.
    public var resolvePromptHandler: ((_ providerID: String, _ nativeID: String, _ approve: Bool) -> ResolveOutcome)?

    private let now: () -> Date
    private let registry: SessionRegistry
    private let broker = PromptBroker()

    private var order: [String] = []                        // registration order (usage display order)
    private var descriptors: [String: ProviderDescriptor] = [:]
    private var enabled: [String: EnabledCapabilities] = [:]
    private var usageByID: [String: ProviderUsage] = [:]
    private var metricsByID: [String: (tokens: Int, contextPct: Int)] = [:]
    private var entries: [String] = []

    private var lastUsage: Usage?
    private var lastBuddy: BuddyState?
    private var lastSessions: [Session] = []

    public init(now: @escaping () -> Date = Date.init, idleTTL: TimeInterval = 300) {
        self.now = now
        self.registry = SessionRegistry(idleTTL: idleTTL)
    }

    // --- registration + toggles ---

    public func register(_ descriptor: ProviderDescriptor, enabled caps: EnabledCapabilities = .all) {
        if descriptors[descriptor.id] == nil { order.append(descriptor.id) }
        descriptors[descriptor.id] = descriptor
        enabled[descriptor.id] = caps
        // Seed usage-capable providers so an enabled provider renders immediately (as "--" until polled),
        // preserving the pre-refactor always-two-cards behavior.
        if descriptor.supportsUsage, usageByID[descriptor.id] == nil { usageByID[descriptor.id] = .unavailable }
        publishUsage(); publishBuddy(); publishSessions()
    }

    public func setEnabled(_ id: String, _ caps: EnabledCapabilities) {
        enabled[id] = caps
        publishUsage(); publishBuddy(); publishSessions()
    }

    public func enabledCaps(for id: String) -> EnabledCapabilities { enabled[id] ?? .all }

    private func usageEnabled(_ id: String) -> Bool {
        (descriptors[id]?.supportsUsage ?? false) && (enabled[id]?.usage ?? true)
    }
    private func buddyEnabled(_ id: String) -> Bool {
        (descriptors[id]?.supportsBuddy ?? false) && (enabled[id]?.buddy ?? true)
    }

    // --- device command routing ---

    public func resolve(shortId: String, approve: Bool) -> ResolveOutcome {
        switch broker.routeForResolve(shortId) {
        case .front(let providerID, let nativeID):
            // The provider fulfills its held connection and calls didEndPrompt, which removes the prompt
            // from the broker and republishes; here we only relay its truthful outcome for the ack.
            return resolvePromptHandler?(providerID, nativeID, approve) ?? .unknown
        case .notFront: return .unknown   // device only shows the front; a queued-id decision is illegitimate
        case .late:     return .late
        case .unknown:  return .unknown
        }
    }

    // Resolve a device `open` short id to its owning provider + native session key. The app runs the
    // focus off-main and acks; nil => unknown_session.
    public func sessionRoute(shortId: String) -> (providerID: String, nativeKey: String)? {
        registry.route(shortId: shortId)
    }

    // Periodic TTL prune (app timer): drop silent sessions + expired prompt tombstones, republish deltas.
    public func reap() { publishBuddy(); publishSessions() }

    // --- ProviderSink ---

    public func provider(_ id: String, didUpdateUsage usage: ProviderUsage) {
        usageByID[id] = usage
        publishUsage()
    }

    public func provider(_ id: String, didUpdateMetrics tokens: Int, contextPct: Int) {
        metricsByID[id] = (tokens, contextPct)
        publishBuddy()
    }

    public func provider(_ id: String, didUpdateSession event: ProviderSessionEvent) {
        let t = now()
        switch event {
        case .activity(let key, let cwd):
            registry.touchActivity(providerID: id, sessionId: key, cwd: cwd, now: t)
        case .stop(let key, let cwd):
            registry.touchActivity(providerID: id, sessionId: key, cwd: cwd, now: t)
            registry.markStop(providerID: id, sessionId: key, now: t)
        case .needsInput(let key, let cwd):
            registry.touchActivity(providerID: id, sessionId: key, cwd: cwd, now: t)
            registry.markNeedsInput(providerID: id, sessionId: key)
        case .branch(let key, let branch):
            registry.setBranch(providerID: id, sessionId: key, branch: branch)
        case .end(let key):
            registry.end(providerID: id, sessionId: key)
        }
        publishBuddy(); publishSessions()
    }

    public func provider(_ id: String, didRaisePrompt nativeID: String, tool: String, hint: String,
                         sessionNativeKey: String?) {
        // A held prompt is liveness + a blocked session; register it so it shows as .waiting even if this
        // is the session's first signal (Codex B2 parity).
        if let key = sessionNativeKey, !key.isEmpty {
            registry.touchActivity(providerID: id, sessionId: key, cwd: nil, now: now())
        }
        let sessionKey = sessionNativeKey.map { SessionRegistry.key(providerID: id, nativeKey: $0) }
        broker.raise(providerID: id, nativeID: nativeID, tool: tool, hint: hint, sessionKey: sessionKey)
        publishBuddy(); publishSessions()
    }

    public func provider(_ id: String, didEndPrompt nativeID: String) {
        _ = broker.end(providerID: id, nativeID: nativeID, now: now())
        publishBuddy(); publishSessions()
    }

    public func provider(_ id: String, didAppendEntry line: String) {
        entries = Array(([line] + entries).prefix(3))   // newest-first, cap 3 (device BUDDY_ENTRIES)
        publishBuddy()
    }

    // --- emit ---

    private func publishUsage() {
        let providers = order.compactMap { id -> UsageEntry? in
            guard usageEnabled(id), let d = descriptors[id], let u = usageByID[id] else { return nil }
            return UsageEntry(id: d.id, label: d.label, h5: u.h5, d7: u.d7, stale: u.stale)
        }
        let merged = Usage(providers: providers)
        guard merged != lastUsage else { return }
        lastUsage = merged
        onUsage?(merged)
    }

    private func publishBuddy() {
        var buddy = BuddyState()
        buddy.running = registry.trackedCount(includeProvider: { [weak self] in self?.buddyEnabled($0) ?? false })
        buddy.waiting = broker.heldSessionKeys(where: { [weak self] in self?.buddyEnabled($0) ?? false }).count
        buddy.tokens = order.reduce(0) { $0 + (buddyEnabled($1) ? (metricsByID[$1]?.tokens ?? 0) : 0) }
        buddy.contextPct = order.compactMap { buddyEnabled($0) ? metricsByID[$0]?.contextPct : nil }.max() ?? 0
        buddy.entries = entries
        if let (fid, h) = broker.front(), buddyEnabled(h.providerID) {
            let n = broker.qlen
            buddy.prompt = BuddyPrompt(id: fid, tool: h.tool, hint: h.hint,
                                       qlen: n > 1 ? n : nil, agent: h.providerID)
        }
        guard buddy != lastBuddy else { return }
        lastBuddy = buddy
        onBuddy?(buddy)
    }

    private func publishSessions() {
        registry.reap(now: now())
        broker.reap(now: now())
        let (front, queued) = broker.waitingSessions()
        let snap = registry.snapshot(now: now(), waitingFront: front, waitingQueued: queued,
                                     includeProvider: { [weak self] in self?.buddyEnabled($0) ?? false })
        guard snap != lastSessions else { return }
        let prevAttention = lastSessions.contains { $0.state == .attention }
        let nowAttention = snap.contains { $0.state == .attention }
        lastSessions = snap
        if !prevAttention && nowAttention { onAttention?() }
        onSessions?(snap)
    }
}
