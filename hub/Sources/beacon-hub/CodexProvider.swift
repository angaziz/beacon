import Foundation
import BeaconHubKit

// Codex is a usage-only provider (issue #160): it polls ~/.codex/auth.json -> wham/usage (CONTRACT.md
// C.2) and has no buddy plane, so Settings shows no Coding Buddy toggle or hook setup for it. Claude
// Code is the only coding-buddy provider.
final class CodexProvider: AgentProvider {
    let descriptor = ProviderDescriptor(id: "codex", label: "CODEX", capabilities: [.usage])
    let usageSource: UsageProvider?

    init(usageSession: URLSession) {
        usageSource = CodexUsageProvider(session: usageSession)
    }

    func start(sink: ProviderSink) {}
    func setEnabled(_ caps: EnabledCapabilities) {}
    func stop() {}
}
