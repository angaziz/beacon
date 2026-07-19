import Foundation

// Global prompt short-id broker (design 2026-07-19). Providers hold their own hook connections open;
// the broker owns the single cross-provider FIFO that decides which prompt is shown on the device
// (`front`) and the depth badge (`qlen`). Pure so the ordering/qlen/late-ack semantics are host-tested.
// Minted ids are `p<n>-<uuid8>` (<= 23 chars, under records.h BUDDY_ID_LEN=24), globally unique.
public final class PromptBroker {
    public struct Held: Equatable {
        public let providerID: String
        public let nativeID: String
        public let tool: String
        public let hint: String
        public let sessionKey: String?   // composite SessionRegistry key of the owning session, if any
    }

    // How a device decision for a short id routes. `notFront` mirrors the current reject-non-front rule
    // (the device only ever shows the front, so a decision for a queued id is illegitimate).
    public enum Route: Equatable {
        case front(providerID: String, nativeID: String)
        case notFront
        case late      // id was minted but already resolved (cap/superseded/withdrawn)
        case unknown   // id never minted
    }

    private var order: [String] = []            // FIFO of live short ids; order.first == device front
    private var held: [String: Held] = [:]      // short id -> live prompt
    private struct Tomb { let at: Date }
    private var tombstones: [String: Tomb] = [:]   // resolved short ids kept briefly for late-ack -> .late
    private var counter: UInt32 = 0
    private static let tombstoneTTL: TimeInterval = 30

    public init() {}

    private func mintId() -> String {
        counter &+= 1
        return "p\(counter)-\(UUID().uuidString.prefix(8))"
    }

    // Append a held prompt to the FIFO; returns its minted short id.
    @discardableResult
    public func raise(providerID: String, nativeID: String, tool: String, hint: String,
                      sessionKey: String?) -> String {
        let id = mintId()
        held[id] = Held(providerID: providerID, nativeID: nativeID, tool: tool, hint: hint,
                        sessionKey: sessionKey)
        order.append(id)
        return id
    }

    // Remove a prompt (front or queued) by its native identity -- the provider knows only its nativeID.
    // Any live/queued prompt leaving (device decision, 590s cap, withdraw-on-close, drain, buddy-off)
    // funnels through here. Tombstones the short id so a racing device decision reports .late, not
    // .unknown. Returns whether something was removed.
    @discardableResult
    public func end(providerID: String, nativeID: String, now: Date = Date()) -> Bool {
        guard let id = order.first(where: { held[$0].map { $0.providerID == providerID && $0.nativeID == nativeID } ?? false })
        else { return false }
        held.removeValue(forKey: id)
        order.removeAll { $0 == id }
        tombstones[id] = Tomb(at: now)
        return true
    }

    public var qlen: Int { order.count }

    public func front() -> (id: String, held: Held)? {
        guard let id = order.first, let h = held[id] else { return nil }
        return (id, h)
    }

    // Route a device decision for a short id (front-guard + late/unknown classification).
    public func routeForResolve(_ shortId: String) -> Route {
        if let h = held[shortId] {
            return order.first == shortId ? .front(providerID: h.providerID, nativeID: h.nativeID) : .notFront
        }
        return tombstones[shortId] != nil ? .late : .unknown
    }

    // Sessions blocked on a user decision, as composite registry keys: the front prompt's session (=>
    // .waiting) and every queued prompt's session (=> .waiting_queued), the front removed from queued.
    public func waitingSessions() -> (front: String?, queued: Set<String>) {
        guard let firstId = order.first, let fp = held[firstId] else { return (nil, []) }
        let front = fp.sessionKey
        var queued = Set<String>()
        for id in order.dropFirst() {
            if let key = held[id]?.sessionKey { queued.insert(key) }
        }
        if let front { queued.remove(front) }
        return (front, queued)
    }

    // Distinct sessions with at least one held prompt, scoped to providers the caller still counts
    // (BuddyState.waiting counts across buddy-enabled providers).
    public func heldSessionKeys(where include: (String) -> Bool) -> Set<String> {
        Set(order.compactMap { id -> String? in
            guard let h = held[id], include(h.providerID), let key = h.sessionKey else { return nil }
            return key
        })
    }

    public func reap(now: Date) {
        let cutoff = now.addingTimeInterval(-Self.tombstoneTTL)
        for (id, t) in tombstones where t.at < cutoff { tombstones.removeValue(forKey: id) }
    }
}
