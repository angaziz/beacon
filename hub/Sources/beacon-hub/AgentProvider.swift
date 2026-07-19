import Foundation
import BeaconHubKit

// A compiled-in agent provider (design 2026-07-19). Each provider declares its capabilities, feeds the
// mux through a ProviderSink, and answers device commands for its own native ids. Usage polling is an
// optional plane: a provider with the `usage` capability exposes a UsageProvider source plus the
// per-provider poll gate (Claude gates its oauth call on statusline freshness; most providers do not).
protocol AgentProvider: AnyObject {
    var descriptor: ProviderDescriptor { get }
    func start(sink: ProviderSink)                 // sink = mux-facing event callbacks
    func setEnabled(_ caps: EnabledCapabilities)   // live toggle; stops holding prompts / polling as needed
    func stop()
    func resolvePrompt(nativeID: String, approve: Bool) -> ResolveOutcome
    func focusSession(nativeKey: String) -> Bool   // sessions capability; false = unsupported / failed

    // Usage plane. nil source => the provider has no usage capability (nothing to poll).
    var usageSource: UsageProvider? { get }
    func shouldPollUsage(now: Date, interval: TimeInterval) -> Bool   // gate this tick (default: always)
    func noteUsageOutcome(_ outcome: ProviderOutcome)                 // update per-provider backoff
}

// Sensible defaults so a provider only implements the planes it supports.
extension AgentProvider {
    var usageSource: UsageProvider? { nil }
    func shouldPollUsage(now: Date, interval: TimeInterval) -> Bool { true }
    func noteUsageOutcome(_ outcome: ProviderOutcome) {}
    func resolvePrompt(nativeID: String, approve: Bool) -> ResolveOutcome { .unknown }
    func focusSession(nativeKey: String) -> Bool { false }
}
