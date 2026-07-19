import Foundation

// Pluggable-provider model (design 2026-07-19). A provider is a named agent ecosystem (claude, codex,
// ...) declaring which capabilities it supports; the user toggles Usage / Coding Buddy per provider.
// Everything here is pure so the whole toggle + settings surface is host-tested; the app target adds
// the UserDefaults conformance and the concrete AgentProvider implementations.

// A provider's supported planes. Tiered: any subset is legal.
public struct ProviderCapabilities: OptionSet, Equatable, Sendable {
    public let rawValue: Int
    public init(rawValue: Int) { self.rawValue = rawValue }
    public static let usage    = ProviderCapabilities(rawValue: 1 << 0)   // quota windows
    public static let sessions = ProviderCapabilities(rawValue: 1 << 1)   // live session list
    public static let prompts  = ProviderCapabilities(rawValue: 1 << 2)   // remote approve/deny
    // The "Coding Buddy" user toggle governs sessions + prompts together, so buddy = both.
    public static let buddy: ProviderCapabilities = [.sessions, .prompts]
}

// Identity + capabilities of one provider. `id`/`label` cross the wire (usage.providers[]); the caps
// gate which toggles the hub shows and which mux planes the provider feeds.
public struct ProviderDescriptor: Equatable, Sendable {
    public let id: String        // stable lowercase ascii, <=12 chars (USAGE_ID_LEN)
    public let label: String     // display string, <=10 chars, uppercase preferred (USAGE_LABEL_LEN)
    public let capabilities: ProviderCapabilities
    public init(id: String, label: String, capabilities: ProviderCapabilities) {
        self.id = id; self.label = label; self.capabilities = capabilities
    }
    public var supportsUsage: Bool { capabilities.contains(.usage) }
    // buddy is offerable iff the provider supports either buddy plane (sessions or prompts).
    public var supportsBuddy: Bool { !capabilities.isDisjoint(with: .buddy) }
}

// The two user toggles, persisted per provider. Defaults ON (preserves current behavior on upgrade).
public struct EnabledCapabilities: Equatable, Sendable {
    public var usage: Bool
    public var buddy: Bool
    public init(usage: Bool = true, buddy: Bool = true) { self.usage = usage; self.buddy = buddy }
    public static let all = EnabledCapabilities(usage: true, buddy: true)
}

// Outcome of resolving a device prompt decision. Shared by the mux (routing/late-ack) and the concrete
// AgentProvider (which fulfills the held hook connection).
public enum ResolveOutcome: Equatable, Sendable {
    case applied   // decision fulfilled the held prompt
    case late      // id known but already resolved (cap/superseded) -> ack(ok:false)
    case unknown   // id never minted -> err(unknown_prompt_id)
}

// The minimal key-value surface ProviderSettings needs. UserDefaults conforms in the app target, so
// this file stays Foundation-only and unit-testable with an in-memory store.
public protocol KeyValueStore: AnyObject {
    func settingsBool(forKey key: String) -> Bool
    func settingsHasValue(forKey key: String) -> Bool
    func settingsSet(_ value: Bool, forKey key: String)
}

// Pure persistence of the per-provider toggles. Absent key => enabled (upgrade default). Only the two
// booleans are stored; capability support is a descriptor concern, checked by the caller before showing
// a toggle.
public struct ProviderSettings {
    private let store: KeyValueStore
    public init(store: KeyValueStore) { self.store = store }

    private static func usageKey(_ id: String) -> String { "BeaconProvider.\(id).usage" }
    private static func buddyKey(_ id: String) -> String { "BeaconProvider.\(id).buddy" }

    public func enabled(for id: String) -> EnabledCapabilities {
        EnabledCapabilities(usage: read(Self.usageKey(id)), buddy: read(Self.buddyKey(id)))
    }
    public func setUsage(_ on: Bool, for id: String) { store.settingsSet(on, forKey: Self.usageKey(id)) }
    public func setBuddy(_ on: Bool, for id: String) { store.settingsSet(on, forKey: Self.buddyKey(id)) }

    // Defaults ON when the user has never set the toggle (absent key), else the stored value.
    private func read(_ key: String) -> Bool {
        store.settingsHasValue(forKey: key) ? store.settingsBool(forKey: key) : true
    }
}
