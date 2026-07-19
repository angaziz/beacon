import Foundation
import BeaconHubKit

// The Codex provider (design 2026-07-19). For now it declares USAGE only: usage comes from the existing
// ~/.codex/auth.json poller (CodexUsageProvider), reused unchanged. The buddy plane (sessions + prompts
// via Codex command-type hooks bridged through /codex/hook) is a later task; the route-path seam and the
// descriptor's future capability slot are named here so wiring it in is additive, not a rearchitecture.
final class CodexProvider: AgentProvider {
    // Path the future beacon-codex-hook shim will POST to (LocalIngestServer route). Declared now so the
    // hook wiring lands without touching the server contract. Not registered until buddy caps ship.
    static let hookPath = "/codex/hook"

    // Capabilities: usage only for now. When the Codex buddy adapter lands it adds [.sessions, .prompts]
    // here and registers the hookPath route + prompt handling; the mux and toggles already support it.
    let descriptor = ProviderDescriptor(id: "codex", label: "CODEX", capabilities: [.usage])

    private let usageProvider: CodexUsageProvider

    init(usageSession: URLSession = .shared) {
        self.usageProvider = CodexUsageProvider(session: usageSession)
    }

    func start(sink: ProviderSink) {}   // no live-ingest routes yet (usage is polled, not pushed)
    func setEnabled(_ caps: EnabledCapabilities) {}
    func stop() {}

    var usageSource: UsageProvider? { usageProvider }
    // No poll gate: Codex has no statusline-equivalent liveness source, so it always polls when enabled.
}
