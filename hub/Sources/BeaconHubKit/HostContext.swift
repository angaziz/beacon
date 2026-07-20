import Foundation

// Per-session host context (terminal app + precise focus handle) used for tap-to-open (issue #110).
// Split out of SessionRegistry so a provider owns the host env it received (from its own hook) and
// answers focusSession(nativeKey:) locally, while the mux owns the global short-id registry. Pure so
// the merge semantics (a later empty/nil field must not wipe a previously-captured value) stay tested.
public struct HostContext: Equatable {
    public var app: String?        // TERM_PROGRAM (host terminal)
    public var focusURL: String?   // WARP_FOCUS_URL -- precise session handle (Warp only)
    public var bundleId: String?   // __CFBundleIdentifier
    public var cwd: String
    public init(app: String? = nil, focusURL: String? = nil, bundleId: String? = nil, cwd: String = "") {
        self.app = app; self.focusURL = focusURL; self.bundleId = bundleId; self.cwd = cwd
    }
}

public final class HostContextStore {
    private var map: [String: HostContext] = [:]   // keyed by the provider-native session key
    public init() {}

    // Merge non-empty fields into the entry, creating it if absent. Only a non-empty incoming value
    // overwrites; nil/"" leaves the prior value intact -- so a `compact` event that doesn't re-export
    // WARP_FOCUS_URL can't wipe a previously-captured precise handle (I2).
    public func set(key: String, app: String?, focusURL: String?, bundleId: String?, cwd: String?) {
        guard !key.isEmpty else { return }
        var h = map[key] ?? HostContext()
        if let v = app, !v.isEmpty { h.app = v }
        if let v = focusURL, !v.isEmpty { h.focusURL = v }
        if let v = bundleId, !v.isEmpty { h.bundleId = v }
        if let v = cwd, !v.isEmpty { h.cwd = v }
        map[key] = h
    }

    public func host(for key: String) -> HostContext? { map[key] }
    public func remove(key: String) { map.removeValue(forKey: key) }
}
