import XCTest
@testable import BeaconHubKit

final class ProviderMuxTests: XCTestCase {
    private let t0 = Date(timeIntervalSince1970: 1_719_400_000)

    // A mux wired with a controllable clock + captured outputs, and two providers: claude (full tier)
    // and codex (usage only). Some tests add a second buddy provider "b" for cross-provider prompts.
    private func makeMux(now: @escaping () -> Date) -> (ProviderMux, Box) {
        let mux = ProviderMux(now: now)
        let box = Box()
        mux.onUsage = { box.usage = $0 }
        mux.onBuddy = { box.buddy = $0 }
        mux.onSessions = { box.sessions = $0 }
        mux.onAttention = { box.attentions += 1 }
        mux.register(ProviderDescriptor(id: "claude", label: "CLAUDE", capabilities: [.usage, .sessions, .prompts]))
        mux.register(ProviderDescriptor(id: "codex", label: "CODEX", capabilities: [.usage]))
        return (mux, box)
    }
    private final class Box {
        var usage = Usage(); var buddy = BuddyState(); var sessions: [Session] = []; var attentions = 0
    }
    private func pu(_ pct: Int) -> ProviderUsage {
        ProviderUsage(h5: UsageWindow(pct: pct, reset: 1), d7: UsageWindow(pct: pct, reset: 2))
    }

    // --- usage toggle filtering + order ---

    func testUsageOnlyEnabledProvidersInRegistrationOrder() {
        let (mux, box) = makeMux(now: { self.t0 })
        mux.provider("claude", didUpdateUsage: pu(24))
        mux.provider("codex", didUpdateUsage: pu(1))
        XCTAssertEqual(box.usage.providers.map(\.id), ["claude", "codex"])   // registration order
        XCTAssertEqual(box.usage.providers.first?.label, "CLAUDE")
        XCTAssertEqual(box.usage.providers.first?.h5.pct, 24)

        mux.setEnabled("claude", EnabledCapabilities(usage: false, buddy: true))
        XCTAssertEqual(box.usage.providers.map(\.id), ["codex"], "usage-off provider drops from the array")

        mux.setEnabled("claude", EnabledCapabilities(usage: true, buddy: true))
        XCTAssertEqual(box.usage.providers.map(\.id), ["claude", "codex"], "re-enable restores it")
    }

    // --- session state transitions (table-driven) ---

    func testSessionStates() {
        struct C { let name: String; let setup: (ProviderMux) -> Void; let expect: SessionState }
        let cases: [C] = [
            C(name: "activity => working", setup: { $0.provider("claude", didUpdateSession: .activity(nativeKey: "a", cwd: "/x/api")) }, expect: .working),
            C(name: "stop => attention", setup: {
                $0.provider("claude", didUpdateSession: .activity(nativeKey: "a", cwd: "/x/api"))
                $0.provider("claude", didUpdateSession: .stop(nativeKey: "a", cwd: "/x/api")) }, expect: .attention),
            C(name: "needsInput => question", setup: {
                $0.provider("claude", didUpdateSession: .needsInput(nativeKey: "a", cwd: "/x/api")) }, expect: .question),
            C(name: "prompt => waiting", setup: {
                $0.provider("claude", didRaisePrompt: "n1", tool: "Bash", hint: "x", sessionNativeKey: "a") }, expect: .waiting),
        ]
        for c in cases {
            let (mux, box) = makeMux(now: { self.t0 })
            c.setup(mux)
            XCTAssertEqual(box.sessions.first?.state, c.expect, c.name)
            XCTAssertEqual(box.sessions.first?.agent, "claude", c.name)
        }
    }

    func testSessionMergeOrderNewestFirst() {
        var now = t0
        let (mux, box) = makeMux(now: { now })
        mux.provider("claude", didUpdateSession: .activity(nativeKey: "a", cwd: "/x/api"))
        now = t0.addingTimeInterval(5)
        mux.provider("claude", didUpdateSession: .activity(nativeKey: "b", cwd: "/x/srv"))
        XCTAssertEqual(box.sessions.map { String($0.label.prefix(3)) }, ["srv", "api"])
    }

    func testAttentionFiresOncePerBucketTransition() {
        let (mux, box) = makeMux(now: { self.t0 })
        mux.provider("claude", didUpdateSession: .activity(nativeKey: "a", cwd: "/x/api"))
        mux.provider("claude", didUpdateSession: .activity(nativeKey: "b", cwd: "/x/api"))
        mux.provider("claude", didUpdateSession: .stop(nativeKey: "a", cwd: "/x/api"))   // 0 -> >0: fire
        mux.provider("claude", didUpdateSession: .stop(nativeKey: "b", cwd: "/x/api"))   // already >0: no fire
        XCTAssertEqual(box.attentions, 1)
    }

    // --- prompt FIFO + qlen (cross-provider) + short-id routing ---

    func testCrossProviderPromptFifoAndQlen() {
        let (mux, box) = makeMux(now: { self.t0 })
        mux.register(ProviderDescriptor(id: "b", label: "B", capabilities: [.sessions, .prompts]))
        mux.provider("claude", didRaisePrompt: "c1", tool: "Bash", hint: "front", sessionNativeKey: "s")
        mux.provider("b", didRaisePrompt: "b1", tool: "Write", hint: "behind", sessionNativeKey: "t")
        XCTAssertEqual(box.buddy.prompt?.hint, "front", "front stays the first-raised prompt")
        XCTAssertEqual(box.buddy.prompt?.agent, "claude")
        XCTAssertEqual(box.buddy.prompt?.qlen, 2, "qlen counts across providers")

        // Ending the front advances to the next provider's prompt; qlen omitted at 1.
        mux.provider("claude", didEndPrompt: "c1")
        XCTAssertEqual(box.buddy.prompt?.hint, "behind")
        XCTAssertEqual(box.buddy.prompt?.agent, "b")
        XCTAssertNil(box.buddy.prompt?.qlen)
    }

    func testResolveRoutesFrontToOwningProvider() {
        let (mux, box) = makeMux(now: { self.t0 })
        var routed: [(String, String, Bool)] = []
        mux.resolvePromptHandler = { pid, nid, approve in
            routed.append((pid, nid, approve))
            mux.provider(pid, didEndPrompt: nid)   // simulate the provider fulfilling + ending
            return .applied
        }
        mux.provider("claude", didRaisePrompt: "c1", tool: "Bash", hint: "x", sessionNativeKey: "s")
        let shortId = box.buddy.prompt!.id
        XCTAssertEqual(mux.resolve(shortId: shortId, approve: true), .applied)
        XCTAssertEqual(routed.count, 1)
        XCTAssertEqual(routed[0].0, "claude")
        XCTAssertEqual(routed[0].1, "c1")
        XCTAssertTrue(routed[0].2)
        XCTAssertNil(box.buddy.prompt, "resolving the only prompt clears the front")
    }

    func testResolveRejectsNonFrontAndReportsLateThenUnknown() {
        let (mux, box) = makeMux(now: { self.t0 })
        mux.resolvePromptHandler = { pid, nid, _ in mux.provider(pid, didEndPrompt: nid); return .applied }
        mux.provider("claude", didRaisePrompt: "c1", tool: "Bash", hint: "front", sessionNativeKey: "s")
        mux.provider("claude", didRaisePrompt: "c2", tool: "Write", hint: "behind", sessionNativeKey: "s")
        let frontId = box.buddy.prompt!.id

        // A non-front (queued) id is illegitimate: rejected as unknown, queue unchanged.
        XCTAssertEqual(mux.resolve(shortId: "p999-nope", approve: true), .unknown)
        // Resolve the front, then a second decision for the same id is late (tombstoned), not unknown.
        XCTAssertEqual(mux.resolve(shortId: frontId, approve: true), .applied)
        XCTAssertEqual(mux.resolve(shortId: frontId, approve: true), .late)
        XCTAssertEqual(mux.resolve(shortId: "never-minted", approve: true), .unknown)
    }

    // --- buddy toggle filtering ---

    func testBuddyOffExcludesSessionsAndPrompt() {
        let (mux, box) = makeMux(now: { self.t0 })
        mux.provider("claude", didUpdateSession: .activity(nativeKey: "a", cwd: "/x/api"))
        mux.provider("claude", didRaisePrompt: "c1", tool: "Bash", hint: "x", sessionNativeKey: "a")
        XCTAssertEqual(box.sessions.count, 1)
        XCTAssertNotNil(box.buddy.prompt)

        mux.setEnabled("claude", EnabledCapabilities(usage: true, buddy: false))
        XCTAssertTrue(box.sessions.isEmpty, "buddy-off provider's sessions excluded")
        XCTAssertNil(box.buddy.prompt, "buddy-off provider's prompt not shown")
        XCTAssertEqual(box.buddy.running, 0)
    }

    // --- buddy entries toggle filtering ---

    // Entries are gated by the per-provider buddy toggle: buffered while enabled (newest-first), dropped
    // on toggle-off (excluded from frames, future lines ignored), and re-enable shows FRESH lines only --
    // pre-disable entries are not resurrected.
    func testBuddyEntriesFilteredByToggle() {
        let (mux, box) = makeMux(now: { self.t0 })
        mux.provider("claude", didAppendEntry: "10:41 yarn test")
        mux.provider("claude", didAppendEntry: "10:42 git push")
        XCTAssertEqual(box.buddy.entries, ["10:42 git push", "10:41 yarn test"])

        mux.setEnabled("claude", EnabledCapabilities(usage: true, buddy: false))
        XCTAssertEqual(box.buddy.entries, [], "buddy-off drops buffered entries")
        mux.provider("claude", didAppendEntry: "10:43 ignored while off")
        XCTAssertEqual(box.buddy.entries, [], "buddy-off provider's new entries never reach the device")

        mux.setEnabled("claude", EnabledCapabilities(usage: true, buddy: true))
        mux.provider("claude", didAppendEntry: "10:44 fresh")
        XCTAssertEqual(box.buddy.entries, ["10:44 fresh"], "re-enable restores fresh entries only")
    }

    // Toggling one provider off leaves another buddy-enabled provider's entries intact.
    func testBuddyEntriesToggleIsPerProvider() {
        let (mux, box) = makeMux(now: { self.t0 })
        mux.register(ProviderDescriptor(id: "b", label: "B", capabilities: [.sessions, .prompts]))
        mux.provider("claude", didAppendEntry: "c1")
        mux.provider("b", didAppendEntry: "b1")
        XCTAssertEqual(box.buddy.entries, ["b1", "c1"])

        mux.setEnabled("claude", EnabledCapabilities(usage: true, buddy: false))
        XCTAssertEqual(box.buddy.entries, ["b1"], "only the disabled provider's entries drop")
    }

    // --- open routing ---

    func testSessionRouteResolvesShortId() {
        let (mux, box) = makeMux(now: { self.t0 })
        mux.provider("claude", didUpdateSession: .activity(nativeKey: "cc-xyz", cwd: "/x/api"))
        let shortId = box.sessions.first!.id
        let route = mux.sessionRoute(shortId: shortId)
        XCTAssertEqual(route?.providerID, "claude")
        XCTAssertEqual(route?.nativeKey, "cc-xyz")
        XCTAssertNil(mux.sessionRoute(shortId: "s999"))
    }

    // --- buddy metrics across providers ---

    func testMetricsSumTokensMaxContext() {
        let (mux, box) = makeMux(now: { self.t0 })
        mux.register(ProviderDescriptor(id: "b", label: "B", capabilities: [.sessions, .prompts]))
        mux.provider("claude", didUpdateMetrics: 100, contextPct: 40)
        mux.provider("b", didUpdateMetrics: 50, contextPct: 70)
        XCTAssertEqual(box.buddy.tokens, 150)
        XCTAssertEqual(box.buddy.contextPct, 70)
    }
}
