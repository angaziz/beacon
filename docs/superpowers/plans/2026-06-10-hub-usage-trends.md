# Hub Usage Trends Tab — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `Now | Trends` view to the Beacon Hub menubar popover — usage-over-time charts, burn pace with projected cap time, and per-model token/cost estimates — without touching the frozen `"v":1` BLE contract or firmware (GitHub issue #57).

**Architecture:** All deterministic logic (pricing math, transcript parsing, dedup, EMA burn-pace, period bucketing, sample records) lives in the pure, dependency-free `BeaconHubKit` target and is unit-tested on the host. The `beacon-hub` executable target adds three IO-bound subsystems — a 1/min usage-history ring buffer (`UsageHistoryStore`), a background incremental transcript scanner (`TranscriptCostReader`), and a statusline cost field — and a SwiftUI `Trends` tab. The popover-open path never triggers a synchronous transcript scan; it renders cached values published onto `HubViewModel`.

**Tech Stack:** Swift 5.9, SwiftPM, AppKit + SwiftUI (macOS 13), Swift Charts (ships with macOS 13), XCTest (table-driven). No third-party dependencies — matches the existing `hub/` package.

---

## Verified ground truth (do not re-derive)

These were confirmed against the repo and the live machine on 2026-06-10. Build the code to these facts.

**Platform:** `hub/Package.swift:20` declares `.macOS(.v13)`. Swift Charts is available — no `Path` fallback is required. `import Charts` works.

**Existing types (`hub/Sources/BeaconHubKit/Protocol.swift`):**
- `UsageWindow { pct: Int?; reset: Int }` (lines 8-12) — `pct` 0…100 or nil; `reset` is Unix epoch seconds, 0 = unknown.
- `ProviderUsage { h5: UsageWindow; d7: UsageWindow }` (lines 14-21), `.unavailable` static.
- `Usage { claude: ProviderUsage; codex: ProviderUsage }` (lines 23-27).

**Claude transcripts** — `~/.claude/projects/**/*.jsonl` (2,130 files; recursive glob already covers the empty `subagents/` dir, so no special-casing). Each assistant line is a top-level JSON object:
- `type == "assistant"`, `requestId` (e.g. `"req_011Cbu…"`), `uuid`, `message.model`, `message.usage`.
- `message.usage` fields: `input_tokens`, `cache_read_input_tokens`, `output_tokens`, and `cache_creation.{ephemeral_5m_input_tokens, ephemeral_1h_input_tokens}`. (`cache_creation_input_tokens` is the flat sum of the two; use the split sub-object for tiered pricing.) `input_tokens` is the **uncached** input (cache reads/writes are separate fields).
- **Skip** `message.model == "<synthetic>"` (not a real API call).
- **Dedup** on `requestId` (primary), falling back to `uuid` when `requestId` is absent — resumed sessions duplicate events.
- Model strings on disk: `claude-opus-4-8`, `claude-fable-5`, `claude-haiku-4-5-20251001`, and the bare alias `"sonnet"` (7 files — real assistant usage, NOT `<synthetic>`). The pricing table MUST map bare aliases (`sonnet`/`opus`/`haiku`) to the current dated model in addition to the exact keys.

**Codex transcripts** — `~/.codex/sessions/2026/**/*.jsonl` (84 files; ~80 carry token data). Per-line JSON object:
- Token events: `type == "event_msg"`, `payload.type == "token_count"`. Token data at `payload.info`. **`payload.info` is `null` on the first event of a session** — skip those.
- `payload.info.last_token_usage` is the **per-turn delta** with `input_tokens`, `cached_input_tokens`, `output_tokens`, `reasoning_output_tokens`, `total_tokens`. `total_token_usage` is the session cumulative — DO NOT use it for attribution (double-counts).
- **Verified invariant:** in `last_token_usage`, `total_tokens == input_tokens + output_tokens`, and `cached_input_tokens ⊆ input_tokens`, `reasoning_output_tokens ⊆ output_tokens`. So: uncached input = `input_tokens − cached_input_tokens`; billable output = `output_tokens` (reasoning already inside — **never add `reasoning_output_tokens` on top**).
- Model comes from the session's `turn_context` record (`type == "turn_context"`, `payload.model`), which appears once per file and applies to all turns. Currently `gpt-5.5`.
- Backfill source for the history line: `payload.rate_limits.primary` (`window_minutes: 300` → h5) and `payload.rate_limits.secondary` (`window_minutes: 10080` → d7), each with `used_percent` (Double) and `resets_at` (epoch seconds). `primary`/`secondary` are `null` on the same empty events — skip.

**Statusline (`hub/Sources/beacon-hub/ClaudeCodeBridge.swift`):** `handleStatusline(_:)` at line 435 parses `body` (a `[String:Any]` from `JSONSerialization`). It reads `context_window` (441-451) and `rate_limits` (454-459) via `Self.int`/`Self.double` (557-569). Add `cost.total_cost_usd` parsing after the `context_window` block; guard for absence so existing handling is untouched.

**Pricing (per 1M tokens, verified 2026-06-10):**

| Model key | Input | Cache read (0.1×) | Cache write 5m (1.25×) | Cache write 1h (2×) | Output |
|---|---|---|---|---|---|
| `claude-opus-4-8` | 5.00 | 0.50 | 6.25 | 10.00 | 25.00 |
| `claude-sonnet-4-6` (alias `sonnet`) | 3.00 | 0.30 | 3.75 | 6.00 | 15.00 |
| `claude-haiku-4-5-20251001` (alias `haiku`) | 1.00 | 0.10 | 1.25 | 2.00 | 5.00 |
| `claude-fable-5` | 10.00 | 1.00 | 12.50 | 20.00 | 50.00 |
| `gpt-5.5` | 5.00 | 0.50 | — | — | 30.00 |

`gpt-5.5` cached-input rate is $0.50/1M (OpenAI list price); Codex has no cache-write tiers. Unknown models: count tokens, cost = nil (excluded from totals, shown as `-`).

---

## File structure

**New — `BeaconHubKit` (pure logic, unit-tested):**
- `hub/Sources/BeaconHubKit/Pricing.swift` — `ModelPricing`, `PricingTable` (hardcoded + alias resolution).
- `hub/Sources/BeaconHubKit/TranscriptUsage.swift` — value types: `ClaudeUsage`, `CodexUsage`, `ModelDayTokens`, `CostBreakdown`, `ModelCostRow`.
- `hub/Sources/BeaconHubKit/CostMath.swift` — pure `cost(for:claude:)` / `cost(for:codex:)`.
- `hub/Sources/BeaconHubKit/TranscriptParsing.swift` — `ClaudeTranscriptParser.parseLine`, `CodexTranscriptParser.parseLine` / `parseModel` (pure, take `[String:Any]`).
- `hub/Sources/BeaconHubKit/CostAggregator.swift` — dedup + per-(model, local day) bucketing; period queries.
- `hub/Sources/BeaconHubKit/BurnPace.swift` — EMA estimator with reset-discontinuity detection.
- `hub/Sources/BeaconHubKit/UsageHistory.swift` — `UsageSample` record + `ChartSeries` gap-aware series builder.

**New — `beacon-hub` (executable, IO/wiring):**
- `hub/Sources/beacon-hub/UsageHistoryStore.swift` — ring-buffer JSONL under `~/.beacon/`, 1/min sampler, 14d prune, Codex backfill.
- `hub/Sources/beacon-hub/TranscriptCostReader.swift` — incremental per-file cursor scanner; background aggregation.
- `hub/Sources/beacon-hub/TrendsView.swift` — Trends tab SwiftUI (chart card, pace rows, cost card, period selector).
- `hub/Sources/beacon-hub/UsageChart.swift` — Swift Charts dual-line view with gaps + dashed cap line.

**Modified — `beacon-hub`:**
- `hub/Sources/beacon-hub/HubPanel.swift` — add `Now | Trends` segmented control; route content.
- `hub/Sources/beacon-hub/HubViewModel.swift` — tab/period state (persisted), chart series, cost breakdown, pace rows.
- `hub/Sources/beacon-hub/MenubarController.swift` — persist tab/period; setters to publish history/cost/pace.
- `hub/Sources/beacon-hub/AppDelegate.swift` — instantiate store + reader, drive sampler off the poll path, feed the VM.
- `hub/Sources/beacon-hub/ClaudeCodeBridge.swift` — parse `cost.total_cost_usd`.

**New tests — `BeaconHubKitTests`:**
- `PricingTests.swift`, `CostMathTests.swift`, `TranscriptParsingTests.swift`, `CostAggregatorTests.swift`, `BurnPaceTests.swift`, `UsageHistoryTests.swift`.

Build/verify command throughout: `cd hub && swift build` (compile) and `cd hub && swift test` (unit). The signed `.app` is only needed for BLE at runtime, not for these checks.

---

# Phase 1 — Pricing table + cost math (BeaconHubKit)

### Task 1: ModelPricing + PricingTable

**Files:**
- Create: `hub/Sources/BeaconHubKit/Pricing.swift`
- Test: `hub/Tests/BeaconHubKitTests/PricingTests.swift`

- [ ] **Step 1: Write the failing test**

```swift
import XCTest
@testable import BeaconHubKit

final class PricingTests: XCTestCase {
    func testKnownModelsResolve() {
        let cases: [(model: String, input: Double, output: Double)] = [
            ("claude-opus-4-8", 5.0, 25.0),
            ("claude-sonnet-4-6", 3.0, 15.0),
            ("claude-haiku-4-5-20251001", 1.0, 5.0),
            ("claude-fable-5", 10.0, 50.0),
            ("gpt-5.5", 5.0, 30.0),
        ]
        for c in cases {
            let p = PricingTable.shared.pricing(for: c.model)
            XCTAssertNotNil(p, "model=\(c.model)")
            XCTAssertEqual(p?.inputPerM, c.input, "model=\(c.model)")
            XCTAssertEqual(p?.outputPerM, c.output, "model=\(c.model)")
        }
    }

    func testBareAliasesResolveToCurrentModel() {
        // On-disk Claude transcripts carry bare "sonnet" (and may carry "opus"/"haiku").
        let cases: [(alias: String, expectedInput: Double)] = [
            ("sonnet", 3.0), ("opus", 5.0), ("haiku", 1.0),
        ]
        for c in cases {
            XCTAssertEqual(PricingTable.shared.pricing(for: c.alias)?.inputPerM, c.expectedInput, "alias=\(c.alias)")
        }
    }

    func testCacheTierMultipliers() {
        let p = PricingTable.shared.pricing(for: "claude-opus-4-8")!
        XCTAssertEqual(p.cacheReadPerM, 0.5)     // 0.1x input
        XCTAssertEqual(p.cacheWrite5mPerM, 6.25)  // 1.25x input
        XCTAssertEqual(p.cacheWrite1hPerM, 10.0)  // 2x input
    }

    func testUnknownModelIsNil() {
        XCTAssertNil(PricingTable.shared.pricing(for: "gpt-9-imaginary"))
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd hub && swift test --filter PricingTests`
Expected: FAIL — `PricingTable` not defined.

- [ ] **Step 3: Write minimal implementation**

```swift
import Foundation

// Hardcoded per-model list prices, per 1,000,000 tokens (issue #57). Cache tiers derive from the input
// rate by the published multipliers: read 0.1x, ephemeral-5m write 1.25x, ephemeral-1h write 2x. Prices
// drift; this table is maintained by hand. Verified 2026-06-10 against platform.claude.com and
// openai.com/api/pricing. Unknown models return nil => counted but cost shown as "-" and excluded from totals.
public struct ModelPricing: Equatable {
    public let inputPerM: Double
    public let cacheReadPerM: Double
    public let cacheWrite5mPerM: Double
    public let cacheWrite1hPerM: Double
    public let outputPerM: Double

    // Claude convention: cache tiers are fixed multiples of the input rate.
    static func claude(input: Double, output: Double) -> ModelPricing {
        ModelPricing(inputPerM: input, cacheReadPerM: input * 0.1,
                     cacheWrite5mPerM: input * 1.25, cacheWrite1hPerM: input * 2.0,
                     outputPerM: output)
    }
    // Codex/OpenAI: published cached-input rate, no cache-write tiers (unused for gpt-5.5).
    static func openai(input: Double, cachedInput: Double, output: Double) -> ModelPricing {
        ModelPricing(inputPerM: input, cacheReadPerM: cachedInput,
                     cacheWrite5mPerM: 0, cacheWrite1hPerM: 0, outputPerM: output)
    }
}

public struct PricingTable {
    public static let shared = PricingTable()

    // Exact model strings verified in local transcripts, plus bare aliases Claude Code can emit
    // ("sonnet" seen on disk; "opus"/"haiku" added defensively). Alias maps to the current dated model.
    private let table: [String: ModelPricing] = [
        "claude-opus-4-8":            .claude(input: 5.0,  output: 25.0),
        "claude-sonnet-4-6":          .claude(input: 3.0,  output: 15.0),
        "claude-haiku-4-5-20251001":  .claude(input: 1.0,  output: 5.0),
        "claude-fable-5":             .claude(input: 10.0, output: 50.0),
        "gpt-5.5":                    .openai(input: 5.0, cachedInput: 0.5, output: 30.0),
    ]
    private let aliases: [String: String] = [
        "opus":   "claude-opus-4-8",
        "sonnet": "claude-sonnet-4-6",
        "haiku":  "claude-haiku-4-5-20251001",
        "fable":  "claude-fable-5",
    ]

    public func pricing(for model: String) -> ModelPricing? {
        if let p = table[model] { return p }
        if let canonical = aliases[model] { return table[canonical] }
        return nil
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd hub && swift test --filter PricingTests`
Expected: PASS (4 tests).

- [ ] **Step 5: Commit**

```bash
git add hub/Sources/BeaconHubKit/Pricing.swift hub/Tests/BeaconHubKitTests/PricingTests.swift
git commit -m "feat(hub): add per-model pricing table with cache tiers and aliases"
```

---

### Task 2: Usage value types + cost math

**Files:**
- Create: `hub/Sources/BeaconHubKit/TranscriptUsage.swift`
- Create: `hub/Sources/BeaconHubKit/CostMath.swift`
- Test: `hub/Tests/BeaconHubKitTests/CostMathTests.swift`

- [ ] **Step 1: Write the failing test**

```swift
import XCTest
@testable import BeaconHubKit

final class CostMathTests: XCTestCase {
    // Claude: input + cacheRead(0.1x) + 5m(1.25x) + 1h(2x) + output, all per 1M.
    func testClaudeCostWithCacheTiers() {
        let usage = ClaudeUsage(inputTokens: 1_000_000, cacheReadTokens: 1_000_000,
                                cacheWrite5mTokens: 1_000_000, cacheWrite1hTokens: 1_000_000,
                                outputTokens: 1_000_000)
        let p = PricingTable.shared.pricing(for: "claude-opus-4-8")!
        // 5 + 0.5 + 6.25 + 10 + 25 = 46.75
        XCTAssertEqual(CostMath.cost(claude: usage, pricing: p), 46.75, accuracy: 1e-9)
    }

    func testClaudeFractionalTokens() {
        let usage = ClaudeUsage(inputTokens: 12_902, cacheReadTokens: 16_371,
                                cacheWrite5mTokens: 0, cacheWrite1hTokens: 3_106, outputTokens: 229)
        let p = PricingTable.shared.pricing(for: "claude-opus-4-8")!
        // (12902*5 + 16371*0.5 + 0*6.25 + 3106*10 + 229*25) / 1e6
        let want = (12_902.0*5 + 16_371*0.5 + 3_106*10 + 229*25) / 1_000_000
        XCTAssertEqual(CostMath.cost(claude: usage, pricing: p), want, accuracy: 1e-9)
    }

    // Codex: uncached input (input-cached) + cached(0.5) + output(reasoning already inside) at output rate.
    func testCodexCostUsesDeltaAndNoDoubleCountReasoning() {
        let usage = CodexUsage(inputTokens: 15_510, cachedInputTokens: 3_456,
                               outputTokens: 426, reasoningOutputTokens: 277)
        let p = PricingTable.shared.pricing(for: "gpt-5.5")!
        // uncached = 12054 @5, cached 3456 @0.5, output 426 @30  (reasoning 277 is INSIDE 426)
        let want = (12_054.0*5 + 3_456*0.5 + 426*30) / 1_000_000
        XCTAssertEqual(CostMath.cost(codex: usage, pricing: p), want, accuracy: 1e-9)
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd hub && swift test --filter CostMathTests`
Expected: FAIL — `ClaudeUsage`/`CostMath` not defined.

- [ ] **Step 3: Write minimal implementation**

`TranscriptUsage.swift`:

```swift
import Foundation

// Per-message Claude token breakdown (from message.usage). Tokens are additive across messages.
public struct ClaudeUsage: Equatable {
    public var inputTokens: Int        // uncached input
    public var cacheReadTokens: Int    // cache_read_input_tokens
    public var cacheWrite5mTokens: Int // cache_creation.ephemeral_5m_input_tokens
    public var cacheWrite1hTokens: Int // cache_creation.ephemeral_1h_input_tokens
    public var outputTokens: Int
    public init(inputTokens: Int = 0, cacheReadTokens: Int = 0, cacheWrite5mTokens: Int = 0,
                cacheWrite1hTokens: Int = 0, outputTokens: Int = 0) {
        self.inputTokens = inputTokens; self.cacheReadTokens = cacheReadTokens
        self.cacheWrite5mTokens = cacheWrite5mTokens; self.cacheWrite1hTokens = cacheWrite1hTokens
        self.outputTokens = outputTokens
    }
    // Total raw tokens for the "X tok" UI label (sum of every billed bucket).
    public var totalTokens: Int { inputTokens + cacheReadTokens + cacheWrite5mTokens + cacheWrite1hTokens + outputTokens }
    public static func + (a: ClaudeUsage, b: ClaudeUsage) -> ClaudeUsage {
        ClaudeUsage(inputTokens: a.inputTokens + b.inputTokens,
                    cacheReadTokens: a.cacheReadTokens + b.cacheReadTokens,
                    cacheWrite5mTokens: a.cacheWrite5mTokens + b.cacheWrite5mTokens,
                    cacheWrite1hTokens: a.cacheWrite1hTokens + b.cacheWrite1hTokens,
                    outputTokens: a.outputTokens + b.outputTokens)
    }
}

// Per-turn Codex token delta (from payload.info.last_token_usage). cached ⊆ input, reasoning ⊆ output.
public struct CodexUsage: Equatable {
    public var inputTokens: Int
    public var cachedInputTokens: Int
    public var outputTokens: Int          // already includes reasoning_output_tokens
    public var reasoningOutputTokens: Int // kept for display only; NOT billed separately
    public init(inputTokens: Int = 0, cachedInputTokens: Int = 0, outputTokens: Int = 0, reasoningOutputTokens: Int = 0) {
        self.inputTokens = inputTokens; self.cachedInputTokens = cachedInputTokens
        self.outputTokens = outputTokens; self.reasoningOutputTokens = reasoningOutputTokens
    }
    public var totalTokens: Int { inputTokens + outputTokens }   // verified invariant
    public static func + (a: CodexUsage, b: CodexUsage) -> CodexUsage {
        CodexUsage(inputTokens: a.inputTokens + b.inputTokens,
                   cachedInputTokens: a.cachedInputTokens + b.cachedInputTokens,
                   outputTokens: a.outputTokens + b.outputTokens,
                   reasoningOutputTokens: a.reasoningOutputTokens + b.reasoningOutputTokens)
    }
}

// A per-model aggregate for one period: raw token count + estimated cost (nil if model unpriced).
public struct ModelCostRow: Equatable {
    public let model: String
    public let tokens: Int
    public let costUSD: Double?   // nil => unpriced model, shown as "-" and excluded from total
    public init(model: String, tokens: Int, costUSD: Double?) {
        self.model = model; self.tokens = tokens; self.costUSD = costUSD
    }
}

// The cost card payload for a selected period: per-model rows + total of the priced rows.
public struct CostBreakdown: Equatable {
    public let rows: [ModelCostRow]       // sorted by cost desc, unpriced last
    public let totalUSD: Double           // sum of priced rows only
    public init(rows: [ModelCostRow], totalUSD: Double) { self.rows = rows; self.totalUSD = totalUSD }
}
```

`CostMath.swift`:

```swift
import Foundation

public enum CostMath {
    public static func cost(claude u: ClaudeUsage, pricing p: ModelPricing) -> Double {
        (Double(u.inputTokens) * p.inputPerM
            + Double(u.cacheReadTokens) * p.cacheReadPerM
            + Double(u.cacheWrite5mTokens) * p.cacheWrite5mPerM
            + Double(u.cacheWrite1hTokens) * p.cacheWrite1hPerM
            + Double(u.outputTokens) * p.outputPerM) / 1_000_000
    }

    public static func cost(codex u: CodexUsage, pricing p: ModelPricing) -> Double {
        let uncachedInput = max(0, u.inputTokens - u.cachedInputTokens)
        return (Double(uncachedInput) * p.inputPerM
            + Double(u.cachedInputTokens) * p.cacheReadPerM
            + Double(u.outputTokens) * p.outputPerM) / 1_000_000   // reasoning already in outputTokens
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd hub && swift test --filter CostMathTests`
Expected: PASS (3 tests).

- [ ] **Step 5: Commit**

```bash
git add hub/Sources/BeaconHubKit/TranscriptUsage.swift hub/Sources/BeaconHubKit/CostMath.swift hub/Tests/BeaconHubKitTests/CostMathTests.swift
git commit -m "feat(hub): add transcript usage types and pure cost math"
```

---

# Phase 2 — Transcript parsing (BeaconHubKit)

### Task 3: Claude + Codex line parsers

**Files:**
- Create: `hub/Sources/BeaconHubKit/TranscriptParsing.swift`
- Test: `hub/Tests/BeaconHubKitTests/TranscriptParsingTests.swift`

Parsers are pure: they take a decoded `[String: Any]` (one JSONL line) and return a typed event or nil. File reading and `JSONSerialization` happen in the executable target (`TranscriptCostReader`), keeping these unit-testable from inline fixtures.

- [ ] **Step 1: Write the failing test**

```swift
import XCTest
@testable import BeaconHubKit

final class TranscriptParsingTests: XCTestCase {
    private func obj(_ json: String) -> [String: Any] {
        try! JSONSerialization.jsonObject(with: Data(json.utf8)) as! [String: Any]
    }

    func testClaudeAssistantLine() {
        let line = obj(#"""
        {"type":"assistant","requestId":"req_1","uuid":"u-1","timestamp":"2026-06-10T11:13:54.726Z",
         "message":{"model":"claude-opus-4-8","usage":{"input_tokens":12902,"cache_read_input_tokens":16371,
         "output_tokens":229,"cache_creation":{"ephemeral_5m_input_tokens":0,"ephemeral_1h_input_tokens":3106}}}}
        """#)
        let e = ClaudeTranscriptParser.parseLine(line)
        XCTAssertEqual(e?.dedupKey, "req_1")
        XCTAssertEqual(e?.model, "claude-opus-4-8")
        XCTAssertEqual(e?.usage.inputTokens, 12902)
        XCTAssertEqual(e?.usage.cacheReadTokens, 16371)
        XCTAssertEqual(e?.usage.cacheWrite1hTokens, 3106)
        XCTAssertEqual(e?.usage.cacheWrite5mTokens, 0)
        XCTAssertEqual(e?.usage.outputTokens, 229)
        XCTAssertEqual(e?.epochSeconds, 1_781_090_034)   // 2026-06-10T11:13:54Z floored (verified)
    }

    func testClaudeFallsBackToUuidWhenNoRequestId() {
        let line = obj(#"{"type":"assistant","uuid":"u-2","timestamp":"2026-06-10T11:13:54Z","message":{"model":"sonnet","usage":{"input_tokens":2,"output_tokens":5}}}"#)
        XCTAssertEqual(ClaudeTranscriptParser.parseLine(line)?.dedupKey, "u-2")
        XCTAssertEqual(ClaudeTranscriptParser.parseLine(line)?.model, "sonnet")
    }

    func testClaudeSkipsSyntheticAndNonAssistant() {
        let synthetic = obj(#"{"type":"assistant","requestId":"r","timestamp":"2026-06-10T11:13:54Z","message":{"model":"<synthetic>","usage":{"input_tokens":1}}}"#)
        XCTAssertNil(ClaudeTranscriptParser.parseLine(synthetic))
        let user = obj(#"{"type":"user","uuid":"x","message":{"model":"claude-opus-4-8"}}"#)
        XCTAssertNil(ClaudeTranscriptParser.parseLine(user))
    }

    func testCodexTokenCountDelta() {
        let line = obj(#"""
        {"type":"event_msg","timestamp":"2026-05-31T14:25:03Z","payload":{"type":"token_count","info":{
          "last_token_usage":{"input_tokens":15510,"cached_input_tokens":3456,"output_tokens":426,
            "reasoning_output_tokens":277,"total_tokens":15936},
          "total_token_usage":{"input_tokens":15510,"output_tokens":426}}}}
        """#)
        let e = CodexTranscriptParser.parseLine(line)
        XCTAssertEqual(e?.usage.inputTokens, 15510)
        XCTAssertEqual(e?.usage.cachedInputTokens, 3456)
        XCTAssertEqual(e?.usage.outputTokens, 426)
        XCTAssertEqual(e?.usage.reasoningOutputTokens, 277)
        XCTAssertEqual(e?.usage.totalTokens, e?.usage.inputTokens! + e!.usage.outputTokens) // 15936
    }

    func testCodexSkipsNullInfo() {
        let line = obj(#"{"type":"event_msg","timestamp":"2026-06-10T11:13:54Z","payload":{"type":"token_count","info":null,"rate_limits":{"primary":null}}}"#)
        XCTAssertNil(CodexTranscriptParser.parseLine(line))
    }

    func testCodexModelFromTurnContext() {
        let line = obj(#"{"type":"turn_context","payload":{"model":"gpt-5.5","cwd":"/x"}}"#)
        XCTAssertEqual(CodexTranscriptParser.parseModel(line), "gpt-5.5")
        XCTAssertNil(CodexTranscriptParser.parseModel(obj(#"{"type":"event_msg","payload":{}}"#)))
    }

    func testCodexBackfillSample() {
        let line = obj(#"""
        {"type":"event_msg","timestamp":"2026-05-31T14:25:09Z","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":1,"output_tokens":1,"total_tokens":2}},
          "rate_limits":{"primary":{"used_percent":1.0,"window_minutes":300,"resets_at":1780230309},
            "secondary":{"used_percent":92.0,"window_minutes":10080,"resets_at":1780230956}}}}
        """#)
        let s = CodexTranscriptParser.parseRateLimits(line)
        XCTAssertEqual(s?.h5Pct, 1)
        XCTAssertEqual(s?.h5Reset, 1_780_230_309)
        XCTAssertEqual(s?.d7Pct, 92)
        XCTAssertEqual(s?.d7Reset, 1_780_230_956)
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd hub && swift test --filter TranscriptParsingTests`
Expected: FAIL — parsers not defined.

- [ ] **Step 3: Write minimal implementation**

```swift
import Foundation

// A parsed, attributable transcript turn. epochSeconds is the floored Unix time of the turn (local-day
// bucketing happens later in CostAggregator). dedupKey survives resumed-session duplication.
public struct ClaudeTurn: Equatable {
    public let dedupKey: String
    public let model: String
    public let epochSeconds: Int
    public let usage: ClaudeUsage
}

public struct CodexTurn: Equatable {
    public let epochSeconds: Int
    public let usage: CodexUsage   // model is per-file (turn_context); attached by the reader
}

// Backfill sample seeded from a Codex token_count's rate_limits (h5=primary 300m, d7=secondary 10080m).
public struct CodexRateSample: Equatable {
    public let epochSeconds: Int
    public let h5Pct: Int?; public let h5Reset: Int
    public let d7Pct: Int?; public let d7Reset: Int
}

private func epoch(fromISO s: String?) -> Int? {
    guard let s else { return nil }
    let f = ISO8601DateFormatter()
    f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
    if let d = f.date(from: s) { return Int(d.timeIntervalSince1970) }
    f.formatOptions = [.withInternetDateTime]
    if let d = f.date(from: s) { return Int(d.timeIntervalSince1970) }
    return nil
}

private func intVal(_ any: Any?) -> Int? {
    if let i = any as? Int { return i }
    if let d = any as? Double { return Int(d) }
    if let s = any as? String, let i = Int(s) { return i }
    return nil
}

public enum ClaudeTranscriptParser {
    public static func parseLine(_ line: [String: Any]) -> ClaudeTurn? {
        guard (line["type"] as? String) == "assistant",
              let message = line["message"] as? [String: Any],
              let model = message["model"] as? String, model != "<synthetic>",
              let usageObj = message["usage"] as? [String: Any] else { return nil }
        guard let key = (line["requestId"] as? String) ?? (line["uuid"] as? String) else { return nil }
        let ts = epoch(fromISO: line["timestamp"] as? String) ?? 0
        let cc = usageObj["cache_creation"] as? [String: Any]
        let usage = ClaudeUsage(
            inputTokens: intVal(usageObj["input_tokens"]) ?? 0,
            cacheReadTokens: intVal(usageObj["cache_read_input_tokens"]) ?? 0,
            cacheWrite5mTokens: intVal(cc?["ephemeral_5m_input_tokens"]) ?? 0,
            cacheWrite1hTokens: intVal(cc?["ephemeral_1h_input_tokens"]) ?? 0,
            outputTokens: intVal(usageObj["output_tokens"]) ?? 0)
        return ClaudeTurn(dedupKey: key, model: model, epochSeconds: ts, usage: usage)
    }
}

public enum CodexTranscriptParser {
    // The model lives in a once-per-file turn_context record.
    public static func parseModel(_ line: [String: Any]) -> String? {
        guard (line["type"] as? String) == "turn_context",
              let payload = line["payload"] as? [String: Any] else { return nil }
        return payload["model"] as? String
    }

    public static func parseLine(_ line: [String: Any]) -> CodexTurn? {
        guard (line["type"] as? String) == "event_msg",
              let payload = line["payload"] as? [String: Any],
              (payload["type"] as? String) == "token_count",
              let info = payload["info"] as? [String: Any],       // null on a session's first event
              let last = info["last_token_usage"] as? [String: Any] else { return nil }
        let ts = epoch(fromISO: line["timestamp"] as? String) ?? 0
        let usage = CodexUsage(
            inputTokens: intVal(last["input_tokens"]) ?? 0,
            cachedInputTokens: intVal(last["cached_input_tokens"]) ?? 0,
            outputTokens: intVal(last["output_tokens"]) ?? 0,
            reasoningOutputTokens: intVal(last["reasoning_output_tokens"]) ?? 0)
        return CodexTurn(epochSeconds: ts, usage: usage)
    }

    public static func parseRateLimits(_ line: [String: Any]) -> CodexRateSample? {
        guard (line["type"] as? String) == "event_msg",
              let payload = line["payload"] as? [String: Any],
              (payload["type"] as? String) == "token_count",
              let rl = payload["rate_limits"] as? [String: Any] else { return nil }
        let primary = rl["primary"] as? [String: Any]
        let secondary = rl["secondary"] as? [String: Any]
        guard primary != nil || secondary != nil else { return nil }   // null on empty events
        func pct(_ d: [String: Any]?) -> Int? {
            guard let v = (d?["used_percent"] as? Double) ?? (d?["used_percent"]).flatMap(intVal).map(Double.init)
            else { return nil }
            return max(0, min(100, Int(v.rounded())))
        }
        let ts = epoch(fromISO: line["timestamp"] as? String) ?? 0
        return CodexRateSample(epochSeconds: ts,
                               h5Pct: pct(primary), h5Reset: intVal(primary?["resets_at"]) ?? 0,
                               d7Pct: pct(secondary), d7Reset: intVal(secondary?["resets_at"]) ?? 0)
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd hub && swift test --filter TranscriptParsingTests`
Expected: PASS (7 tests).

- [ ] **Step 5: Commit**

```bash
git add hub/Sources/BeaconHubKit/TranscriptParsing.swift hub/Tests/BeaconHubKitTests/TranscriptParsingTests.swift
git commit -m "feat(hub): add pure Claude/Codex transcript line parsers"
```

---

# Phase 3 — Cost aggregation + period bucketing (BeaconHubKit)

### Task 4: CostAggregator (dedup + per-model per-day + period queries)

**Files:**
- Create: `hub/Sources/BeaconHubKit/CostAggregator.swift`
- Test: `hub/Tests/BeaconHubKitTests/CostAggregatorTests.swift`

The aggregator owns dedup (across the whole corpus, by key) and bucketing into per-(model, local calendar day) token sums. Period queries (`last 6h`, `today`, `this week`) read the buckets and apply pricing. A `Calendar` is injected so midnight/week boundaries are testable.

- [ ] **Step 1: Write the failing test**

```swift
import XCTest
@testable import BeaconHubKit

final class CostAggregatorTests: XCTestCase {
    private func cal(_ tz: String) -> Calendar {
        var c = Calendar(identifier: .gregorian)
        c.timeZone = TimeZone(identifier: tz)!
        c.firstWeekday = 2   // Monday
        return c
    }

    func testDedupByKey() {
        var agg = CostAggregator(calendar: cal("UTC"))
        let u = ClaudeUsage(inputTokens: 100, outputTokens: 10)
        agg.addClaude(ClaudeTurn(dedupKey: "req_1", model: "claude-opus-4-8", epochSeconds: 1_780_000_000, usage: u))
        agg.addClaude(ClaudeTurn(dedupKey: "req_1", model: "claude-opus-4-8", epochSeconds: 1_780_000_000, usage: u)) // dup
        let bd = agg.breakdown(period: .day, now: 1_780_000_000, table: PricingTable.shared)
        XCTAssertEqual(bd.rows.count, 1)
        XCTAssertEqual(bd.rows.first?.tokens, 110)   // counted once
    }

    func testPeriodBucketingAcrossMidnightLocalTZ() {
        // 23:30 and 00:30 local (Asia/Jakarta, +07) fall on different calendar days.
        let agg = CostAggregator(calendar: cal("Asia/Jakarta"))
        var a = agg
        // 2026-06-10 23:30 +07 (local day Jun 10) == epoch 1781109000 (verified)
        a.addCodex(model: "gpt-5.5", turn: CodexTurn(epochSeconds: 1_781_109_000, usage: CodexUsage(inputTokens: 1000, outputTokens: 10)))
        // 2026-06-11 00:30 +07 (local day Jun 11) == epoch 1781112600 (verified)
        a.addCodex(model: "gpt-5.5", turn: CodexTurn(epochSeconds: 1_781_112_600, usage: CodexUsage(inputTokens: 2000, outputTokens: 20)))
        // "today" at 2026-06-11 01:00 +07 should see only the second turn.
        let now = 1_781_114_400   // 2026-06-11 01:00 +07 (verified)
        let today = a.breakdown(period: .day, now: now, table: PricingTable.shared)
        XCTAssertEqual(today.rows.first?.tokens, 2020)
    }

    func testUnpricedModelExcludedFromTotalShownAsNilCost() {
        var agg = CostAggregator(calendar: cal("UTC"))
        agg.addClaude(ClaudeTurn(dedupKey: "k1", model: "claude-opus-4-8", epochSeconds: 1_780_000_000,
                                 usage: ClaudeUsage(inputTokens: 1_000_000)))
        agg.addClaude(ClaudeTurn(dedupKey: "k2", model: "mystery-model", epochSeconds: 1_780_000_000,
                                 usage: ClaudeUsage(inputTokens: 1_000_000)))
        let bd = agg.breakdown(period: .day, now: 1_780_000_000, table: PricingTable.shared)
        XCTAssertEqual(bd.totalUSD, 5.0, accuracy: 1e-9)   // only the opus row
        XCTAssertNil(bd.rows.first(where: { $0.model == "mystery-model" })?.costUSD)
    }

    func testRollingSixHourWindow() {
        var agg = CostAggregator(calendar: cal("UTC"))
        let now = 1_780_000_000
        agg.addClaude(ClaudeTurn(dedupKey: "old", model: "claude-opus-4-8", epochSeconds: now - 7*3600, usage: ClaudeUsage(inputTokens: 1000)))
        agg.addClaude(ClaudeTurn(dedupKey: "new", model: "claude-opus-4-8", epochSeconds: now - 1*3600, usage: ClaudeUsage(inputTokens: 2000)))
        let bd = agg.breakdown(period: .last6h, now: now, table: PricingTable.shared)
        XCTAssertEqual(bd.rows.first?.tokens, 2000)   // only the within-6h turn
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd hub && swift test --filter CostAggregatorTests`
Expected: FAIL — `CostAggregator`/`CostPeriod` not defined.

- [ ] **Step 3: Write minimal implementation**

```swift
import Foundation

// The three period scopes the cost card supports. last6h is a rolling window; day/week are local
// calendar boundaries ("today" / "this week"). Mapping to chart periods: 6h->last6h, 24h->day, 7d->week.
public enum CostPeriod {
    case last6h, day, week
}

// Aggregates deduped transcript turns into per-(model, localDayStart) token sums, then prices period
// queries on demand. Calendar is injected so midnight/week boundaries are deterministic in tests.
// Token bucketing is provider-agnostic (Claude and Codex tokens both land in ClaudeUsage-shaped sums for
// counting; cost is computed per provider so cache mechanics stay correct).
public struct CostAggregator {
    private let calendar: Calendar
    private var seenKeys = Set<String>()
    // model -> dayStartEpoch -> accumulated tokens + cost contribution
    private struct Bucket { var tokens = 0; var cost = 0.0; var priced = true }
    private var buckets: [String: [Int: Bucket]] = [:]

    public init(calendar: Calendar) { self.calendar = calendar }

    private func dayStart(_ epoch: Int) -> Int {
        let d = Date(timeIntervalSince1970: TimeInterval(epoch))
        return Int(calendar.startOfDay(for: d).timeIntervalSince1970)
    }

    public mutating func addClaude(_ turn: ClaudeTurn) {
        guard seenKeys.insert(turn.dedupKey).inserted else { return }
        let priced = PricingTable.shared.pricing(for: turn.model)
        let cost = priced.map { CostMath.cost(claude: turn.usage, pricing: $0) } ?? 0
        accumulate(model: turn.model, epoch: turn.epochSeconds, tokens: turn.usage.totalTokens,
                   cost: cost, priced: priced != nil)
    }

    public mutating func addCodex(model: String, turn: CodexTurn) {
        // Codex token_count events have no per-event id; dedup is by file cursor in the reader, not here.
        let priced = PricingTable.shared.pricing(for: model)
        let cost = priced.map { CostMath.cost(codex: turn.usage, pricing: $0) } ?? 0
        accumulate(model: model, epoch: turn.epochSeconds, tokens: turn.usage.totalTokens,
                   cost: cost, priced: priced != nil)
    }

    private mutating func accumulate(model: String, epoch: Int, tokens: Int, cost: Double, priced: Bool) {
        let day = dayStart(epoch)
        var byDay = buckets[model] ?? [:]
        var b = byDay[day] ?? Bucket()
        b.tokens += tokens; b.cost += cost; b.priced = priced
        byDay[day] = b
        buckets[model] = byDay
    }

    private func range(for period: CostPeriod, now: Int) -> ClosedRange<Int> {
        let nowDate = Date(timeIntervalSince1970: TimeInterval(now))
        switch period {
        case .last6h:
            return (now - 6*3600)...now
        case .day:
            let start = Int(calendar.startOfDay(for: nowDate).timeIntervalSince1970)
            return start...now
        case .week:
            let comps = calendar.dateComponents([.yearForWeekOfYear, .weekOfYear], from: nowDate)
            let start = calendar.date(from: comps).map { Int($0.timeIntervalSince1970) } ?? now
            return start...now
        }
    }

    public func breakdown(period: CostPeriod, now: Int, table: PricingTable) -> CostBreakdown {
        let r = range(for: period, now: now)
        // day/week query whole-day buckets; last6h needs sub-day precision, so re-fold by inclusion.
        var rows: [ModelCostRow] = []
        var total = 0.0
        for (model, byDay) in buckets {
            var tokens = 0; var cost = 0.0; var priced = true
            for (day, b) in byDay {
                // A day bucket is in-range if its day start is within the period's day span. For last6h the
                // 6h window can straddle midnight, so include any day whose span overlaps [start,now] and
                // rely on the day granularity being acceptable for the 6h scope's bucket reads. (Turn-level
                // precision for last6h is preserved because the reader can also expose a fine path; here we
                // approximate to whole days for day/week and exact for the others via the dedicated index.)
                if dayInRange(day, period: period, range: r) { tokens += b.tokens; cost += b.cost; priced = priced && b.priced }
            }
            if tokens > 0 {
                rows.append(ModelCostRow(model: model, tokens: tokens, costUSD: priced ? cost : nil))
                if priced { total += cost }
            }
        }
        rows.sort { lhs, rhs in (lhs.costUSD ?? -1) > (rhs.costUSD ?? -1) }
        return CostBreakdown(rows: rows, totalUSD: total)
    }

    private func dayInRange(_ dayStart: Int, period: CostPeriod, range: ClosedRange<Int>) -> Bool {
        switch period {
        case .day, .week:
            return dayStart >= range.lowerBound - 0 && dayStart <= range.upperBound
        case .last6h:
            let dayEnd = dayStart + 86_400
            return dayEnd > range.lowerBound && dayStart <= range.upperBound
        }
    }
}
```

> **Note for the reviewer (Task 4):** `last6h` is bucketed to whole days here, which over-counts when the 6h window starts mid-day. The test `testRollingSixHourWindow` will FAIL with this whole-day approximation. Replace the `addClaude`/`addCodex` accumulation with a turn-level secondary index keyed by epoch for exact `last6h`/rolling reads — see Step 3b below. Keep the day buckets for `day`/`week`.

- [ ] **Step 3b: Add a turn-level index so last6h is exact**

Replace the `Bucket`/`buckets`/`accumulate`/`breakdown` machinery with an additional flat list of priced turns so rolling windows are exact while day/week still read fast:

```swift
    // Flat, deduped turn records — exact for any time range. (Corpus is low-thousands of turns; linear
    // scans on popover-period changes are trivial and run off the UI path anyway.)
    private struct Turn { let model: String; let epoch: Int; let tokens: Int; let cost: Double; let priced: Bool }
    private var turns: [Turn] = []

    private mutating func accumulate(model: String, epoch: Int, tokens: Int, cost: Double, priced: Bool) {
        turns.append(Turn(model: model, epoch: epoch, tokens: tokens, cost: cost, priced: priced))
    }

    public func breakdown(period: CostPeriod, now: Int, table: PricingTable) -> CostBreakdown {
        let r = range(for: period, now: now)
        var byModel: [String: (tokens: Int, cost: Double, priced: Bool)] = [:]
        for t in turns where t.epoch >= r.lowerBound && t.epoch <= r.upperBound {
            var acc = byModel[t.model] ?? (0, 0, true)
            acc.tokens += t.tokens; acc.cost += t.cost; acc.priced = acc.priced && t.priced
            byModel[t.model] = acc
        }
        var rows: [ModelCostRow] = []
        var total = 0.0
        for (model, acc) in byModel {
            rows.append(ModelCostRow(model: model, tokens: acc.tokens, costUSD: acc.priced ? acc.cost : nil))
            if acc.priced { total += acc.cost }
        }
        rows.sort { ($0.costUSD ?? -1) > ($1.costUSD ?? -1) }
        return CostBreakdown(rows: rows, totalUSD: total)
    }
```

Delete the now-unused `Bucket`, `buckets`, `dayStart`, and `dayInRange` members.

- [ ] **Step 4: Run test to verify it passes**

Run: `cd hub && swift test --filter CostAggregatorTests`
Expected: PASS (4 tests).

- [ ] **Step 5: Commit**

```bash
git add hub/Sources/BeaconHubKit/CostAggregator.swift hub/Tests/BeaconHubKitTests/CostAggregatorTests.swift
git commit -m "feat(hub): add dedup + period-bucketed cost aggregator"
```

---

# Phase 4 — Burn pace EMA (BeaconHubKit)

### Task 5: BurnPace estimator

**Files:**
- Create: `hub/Sources/BeaconHubKit/BurnPace.swift`
- Test: `hub/Tests/BeaconHubKitTests/BurnPaceTests.swift`

EMA over the last ~20-30 min of 5h-window pct samples per provider. Emits a pace only after ≥3 movement samples. Resets the estimator on a discontinuity: pct drops ≥10 points, the window `reset` epoch changes, or a sample gap > 10 min. ETA = time to 100% at current pace; compared against `reset` to choose warn vs safe styling.

- [ ] **Step 1: Write the failing test**

```swift
import XCTest
@testable import BeaconHubKit

final class BurnPaceTests: XCTestCase {
    // "Show only after >= 3 samples of movement" (issue #57). The baseline (first) sample is not a
    // movement, so 3 movements require 4 updates: nil through the 3rd, a pace on the 4th.
    func testNoPaceUntilThreeMovementSamples() {
        var e = BurnPaceEstimator()
        XCTAssertNil(e.update(pct: 10, reset: 1000, now: 0).pace)   // baseline, 0 movements
        XCTAssertNil(e.update(pct: 12, reset: 1000, now: 60).pace)  // movement 1
        XCTAssertNil(e.update(pct: 14, reset: 1000, now: 120).pace) // movement 2
        XCTAssertNotNil(e.update(pct: 16, reset: 1000, now: 180).pace) // movement 3 => pace
    }

    func testPaceIsRoughlyPercentPerHour() {
        var e = BurnPaceEstimator()
        // +2 pct every 60s == +120 pct/h steady; EMA converges near it.
        _ = e.update(pct: 10, reset: 100000, now: 0)
        _ = e.update(pct: 12, reset: 100000, now: 60)
        _ = e.update(pct: 14, reset: 100000, now: 120)
        let r = e.update(pct: 16, reset: 100000, now: 180)
        XCTAssertEqual(r.pace!, 120, accuracy: 30)   // EMA-smoothed, generous band
    }

    func testResetOnPctDropTenPoints() {
        var e = BurnPaceEstimator()
        _ = e.update(pct: 50, reset: 100000, now: 0)
        _ = e.update(pct: 52, reset: 100000, now: 60)
        _ = e.update(pct: 54, reset: 100000, now: 120)
        let afterReset = e.update(pct: 5, reset: 200000, now: 180)   // window reset: -49 pts AND reset changed
        XCTAssertNil(afterReset.pace)   // estimator cleared, needs 3 fresh movement samples
    }

    func testResetOnSampleGapOverTenMinutes() {
        var e = BurnPaceEstimator()
        _ = e.update(pct: 10, reset: 100000, now: 0)
        _ = e.update(pct: 12, reset: 100000, now: 60)
        _ = e.update(pct: 14, reset: 100000, now: 120)
        let afterGap = e.update(pct: 16, reset: 100000, now: 120 + 11*60)  // >10min gap
        XCTAssertNil(afterGap.pace)
    }

    func testEtaAndCapVsResetStyling() {
        var e = BurnPaceEstimator()
        _ = e.update(pct: 80, reset: 999_999_999, now: 0)
        _ = e.update(pct: 84, reset: 999_999_999, now: 60)
        _ = e.update(pct: 88, reset: 999_999_999, now: 120)
        // 4th update = 3rd movement => pace emitted. reset is far in the future => cap before reset => warn.
        let r = e.update(pct: 92, reset: 999_999_999, now: 180)
        XCTAssertNotNil(r.capEpoch)
        XCTAssertEqual(r.style, .warn)
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd hub && swift test --filter BurnPaceTests`
Expected: FAIL — `BurnPaceEstimator` not defined.

- [ ] **Step 3: Write minimal implementation**

```swift
import Foundation

public struct BurnPaceResult: Equatable {
    public enum Style { case warn, safe }
    public let pace: Double?      // pct per hour; nil until enough movement
    public let capEpoch: Int?     // projected time to 100% at current pace; nil if pace nil or non-positive
    public let style: Style?      // warn = caps before window reset; safe = reaches reset first
}

// Per-provider EMA over recent 5h-window pct samples. Reset on discontinuity (window rollover, big drop,
// long gap) so the estimate never blends across a window boundary. Caller feeds one sample per ~minute.
public struct BurnPaceEstimator {
    private let alpha = 0.4            // EMA smoothing on the per-sample slope
    private let minSamples = 3
    private let dropResetPoints = 10
    private let gapResetSeconds = 600

    private var lastPct: Int?
    private var lastNow: Int?
    private var lastReset: Int?
    private var ema: Double?          // pct/hour
    private var movementSamples = 0

    public init() {}

    public mutating func update(pct: Int, reset: Int, now: Int) -> BurnPaceResult {
        defer { lastPct = pct; lastNow = now; lastReset = reset }

        // Discontinuity detection -> clear and restart.
        if let lp = lastPct, let ln = lastNow, let lr = lastReset {
            let dropped = (lp - pct) >= dropResetPoints
            let resetChanged = reset != lr
            let gapped = (now - ln) > gapResetSeconds
            if dropped || resetChanged || gapped {
                ema = nil; movementSamples = 0
                return BurnPaceResult(pace: nil, capEpoch: nil, style: nil)
            }
            let dtHours = Double(now - ln) / 3600.0
            if dtHours > 0 {
                let slope = Double(pct - lp) / dtHours   // pct/hour for this interval
                ema = ema.map { $0 + alpha * (slope - $0) } ?? slope
                if pct != lp { movementSamples += 1 }
            }
        }

        // Withhold the pace until >= 3 movement samples (return nil, NOT the partial ema).
        guard movementSamples >= minSamples, let pace = ema, pace > 0 else {
            return BurnPaceResult(pace: nil, capEpoch: nil, style: nil)
        }
        let remaining = Double(max(0, 100 - pct))
        let hoursToCap = remaining / pace
        let capEpoch = now + Int((hoursToCap * 3600).rounded())
        let style: BurnPaceResult.Style = (reset > 0 && capEpoch >= reset) ? .safe : .warn
        return BurnPaceResult(pace: pace, capEpoch: capEpoch, style: style)
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd hub && swift test --filter BurnPaceTests`
Expected: PASS (5 tests).

- [ ] **Step 5: Commit**

```bash
git add hub/Sources/BeaconHubKit/BurnPace.swift hub/Tests/BeaconHubKitTests/BurnPaceTests.swift
git commit -m "feat(hub): add EMA burn-pace estimator with discontinuity resets"
```

---

# Phase 5 — Usage history record + gap-aware series (BeaconHubKit)

### Task 6: UsageSample + ChartSeries

**Files:**
- Create: `hub/Sources/BeaconHubKit/UsageHistory.swift`
- Test: `hub/Tests/BeaconHubKitTests/UsageHistoryTests.swift`

`UsageSample` is the ring-buffer record (Codable, written by `UsageHistoryStore`). `ChartSeries` turns a flat sample list into per-provider point arrays for a selected period, inserting **breaks** (segment boundaries) wherever a gap exceeds a threshold so the chart never interpolates across "Hub not running / Mac asleep".

- [ ] **Step 1: Write the failing test**

```swift
import XCTest
@testable import BeaconHubKit

final class UsageHistoryTests: XCTestCase {
    func testSampleRoundTripsJSON() throws {
        let s = UsageSample(ts: 1_780_000_000, provider: .claude, h5: 42, d7: 7)
        let data = try JSONEncoder().encode(s)
        let back = try JSONDecoder().decode(UsageSample.self, from: data)
        XCTAssertEqual(s, back)
    }

    func testSeriesSplitsOnGap() {
        // Samples 1 min apart, then a 20-min gap, then resume. With a 5-min break threshold => 2 segments.
        var samples: [UsageSample] = []
        for i in 0..<3 { samples.append(UsageSample(ts: 1000 + i*60, provider: .claude, h5: 10 + i, d7: 1)) }
        for i in 0..<3 { samples.append(UsageSample(ts: 1000 + 20*60 + i*60, provider: .claude, h5: 20 + i, d7: 2)) }
        let series = ChartSeries.build(samples: samples, provider: .claude, metric: .h5,
                                       range: 0...(1000 + 30*60), breakSeconds: 300)
        XCTAssertEqual(series.segments.count, 2)
        XCTAssertEqual(series.segments[0].count, 3)
        XCTAssertEqual(series.segments[1].count, 3)
    }

    func testSeriesFiltersToRangeAndProvider() {
        let samples = [
            UsageSample(ts: 100, provider: .claude, h5: 5, d7: 1),
            UsageSample(ts: 200, provider: .codex, h5: 9, d7: 2),
            UsageSample(ts: 300, provider: .claude, h5: 7, d7: 1),
        ]
        let series = ChartSeries.build(samples: samples, provider: .claude, metric: .h5, range: 150...400, breakSeconds: 300)
        XCTAssertEqual(series.segments.flatMap { $0 }.map { $0.pct }, [7])
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd hub && swift test --filter UsageHistoryTests`
Expected: FAIL — `UsageSample`/`ChartSeries` not defined.

- [ ] **Step 3: Write minimal implementation**

```swift
import Foundation

public enum UsageProviderKind: String, Codable, Equatable { case claude, codex }

// One ring-buffer sample (~1/min). Compact keys keep the JSONL file small (low single-digit MB over 14d).
public struct UsageSample: Codable, Equatable {
    public let ts: Int                 // epoch seconds
    public let provider: UsageProviderKind
    public let h5: Int?                // 5h-window pct
    public let d7: Int?                // 7d-window pct
    public init(ts: Int, provider: UsageProviderKind, h5: Int?, d7: Int?) {
        self.ts = ts; self.provider = provider; self.h5 = h5; self.d7 = d7
    }
    enum CodingKeys: String, CodingKey { case ts = "t", provider = "p", h5, d7 }
}

public enum ChartMetric { case h5, d7 }

public struct ChartPoint: Equatable { public let ts: Int; public let pct: Int }

// Gap-aware series: a list of contiguous segments. The chart draws each segment as its own line so a gap
// (Hub down / Mac asleep) renders as a break, never an interpolated diagonal.
public struct ChartSeries: Equatable {
    public let segments: [[ChartPoint]]

    public static func build(samples: [UsageSample], provider: UsageProviderKind, metric: ChartMetric,
                             range: ClosedRange<Int>, breakSeconds: Int) -> ChartSeries {
        let points: [ChartPoint] = samples
            .filter { $0.provider == provider && $0.ts >= range.lowerBound && $0.ts <= range.upperBound }
            .sorted { $0.ts < $1.ts }
            .compactMap { s in
                let pct = metric == .h5 ? s.h5 : s.d7
                return pct.map { ChartPoint(ts: s.ts, pct: $0) }
            }
        var segments: [[ChartPoint]] = []
        var current: [ChartPoint] = []
        for p in points {
            if let last = current.last, p.ts - last.ts > breakSeconds {
                segments.append(current); current = []
            }
            current.append(p)
        }
        if !current.isEmpty { segments.append(current) }
        return ChartSeries(segments: segments)
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd hub && swift test --filter UsageHistoryTests`
Expected: PASS (3 tests).

- [ ] **Step 5: Commit**

```bash
git add hub/Sources/BeaconHubKit/UsageHistory.swift hub/Tests/BeaconHubKitTests/UsageHistoryTests.swift
git commit -m "feat(hub): add usage-sample record and gap-aware chart series builder"
```

---

# Phase 6 — UsageHistoryStore (executable: ring buffer + sampler + backfill)

### Task 7: UsageHistoryStore

**Files:**
- Create: `hub/Sources/beacon-hub/UsageHistoryStore.swift`

This is IO and not host-unit-tested (it touches `~/.beacon` and `~/.codex`); the pure series/sample logic it relies on is already covered in Task 6. Verify by compile + a manual smoke check.

- [ ] **Step 1: Write the store**

```swift
import Foundation
import BeaconHubKit

// Append-only ring buffer of UsageSample under ~/.beacon/usage-history.jsonl, sampled ~1/min from the
// existing poll pipeline. Retention 14 days; pruned on launch and on day rollover. On first run (file
// absent) the Codex line is backfilled from existing ~/.codex/sessions rate_limits; Claude is forward-only.
// All disk IO runs on a private queue; loaded samples are cached in memory for the chart reads.
final class UsageHistoryStore {
    private let url: URL
    private let queue = DispatchQueue(label: "beacon.history")
    private let retention: TimeInterval = 14 * 86_400
    private var samples: [UsageSample] = []
    private var lastSampleTs: [UsageProviderKind: Int] = [:]

    init() {
        let dir = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".beacon", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        url = dir.appendingPathComponent("usage-history.jsonl")
    }

    // Call once at launch. Loads + prunes; backfills Codex if the file was absent.
    func start() {
        queue.async {
            let existed = FileManager.default.fileExists(atPath: self.url.path)
            self.load()
            if !existed { self.backfillCodex() }
            self.prune()
        }
    }

    // Returns a snapshot for the chart (main thread reads this via the AppDelegate plumbing).
    func snapshot(_ completion: @escaping ([UsageSample]) -> Void) {
        queue.async { let s = self.samples; DispatchQueue.main.async { completion(s) } }
    }

    // Feed from the poll pipeline. Rate-limited to ~1/min per provider so the file stays small.
    func record(_ usage: Usage, now: Date = Date()) {
        queue.async {
            let ts = Int(now.timeIntervalSince1970)
            self.appendIfDue(.claude, h5: usage.claude.h5.pct, d7: usage.claude.d7.pct, ts: ts)
            self.appendIfDue(.codex, h5: usage.codex.h5.pct, d7: usage.codex.d7.pct, ts: ts)
        }
    }

    private func appendIfDue(_ provider: UsageProviderKind, h5: Int?, d7: Int?, ts: Int) {
        if let last = lastSampleTs[provider], ts - last < 55 { return }   // ~1/min, small slack
        let s = UsageSample(ts: ts, provider: provider, h5: h5, d7: d7)
        samples.append(s)
        lastSampleTs[provider] = ts
        appendLine(s)
        if Int.random(in: 0..<60) == 0 { prune() }   // occasional rollover prune; cheap relative to writes
    }

    private func appendLine(_ s: UsageSample) {
        guard let data = try? JSONEncoder().encode(s) else { return }
        var line = data; line.append(0x0A)
        if let h = try? FileHandle(forWritingTo: url) {
            defer { try? h.close() }
            h.seekToEndOfFile(); h.write(line)
        } else {
            try? line.write(to: url)   // file did not exist
        }
    }

    private func load() {
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return }
        let dec = JSONDecoder()
        samples = text.split(separator: "\n").compactMap { line in
            try? dec.decode(UsageSample.self, from: Data(line.utf8))
        }
        for s in samples { lastSampleTs[s.provider] = max(lastSampleTs[s.provider] ?? 0, s.ts) }
    }

    // Drop samples older than retention, then rewrite the file compactly.
    private func prune() {
        let cutoff = Int(Date().timeIntervalSince1970 - retention)
        let kept = samples.filter { $0.ts >= cutoff }
        guard kept.count != samples.count else { return }
        samples = kept
        let enc = JSONEncoder()
        var blob = Data()
        for s in kept { if let d = try? enc.encode(s) { blob.append(d); blob.append(0x0A) } }
        try? blob.write(to: url, options: .atomic)
    }

    // First-run only: seed the Codex history line from existing session files' rate_limits, so the 7d/24h
    // charts have Codex context immediately. Claude has no equivalent on-disk pct history (forward-only).
    private func backfillCodex() {
        let root = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".codex/sessions", isDirectory: true)
        guard let en = FileManager.default.enumerator(at: root, includingPropertiesForKeys: nil) else { return }
        var seeded: [UsageSample] = []
        let cutoff = Int(Date().timeIntervalSince1970 - retention)
        for case let f as URL in en where f.pathExtension == "jsonl" {
            guard let text = try? String(contentsOf: f, encoding: .utf8) else { continue }
            for line in text.split(separator: "\n") {
                guard let obj = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: Any],
                      let r = CodexTranscriptParser.parseRateLimits(obj), r.epochSeconds >= cutoff else { continue }
                seeded.append(UsageSample(ts: r.epochSeconds, provider: .codex, h5: r.h5Pct, d7: r.d7Pct))
            }
        }
        guard !seeded.isEmpty else { return }
        seeded.sort { $0.ts < $1.ts }
        samples = (samples + seeded).sorted { $0.ts < $1.ts }
        // Rewrite once with the seeded history merged in.
        let enc = JSONEncoder()
        var blob = Data()
        for s in samples { if let d = try? enc.encode(s) { blob.append(d); blob.append(0x0A) } }
        try? blob.write(to: url, options: .atomic)
    }
}
```

- [ ] **Step 2: Verify compile**

Run: `cd hub && swift build`
Expected: Builds with no errors.

- [ ] **Step 3: Commit**

```bash
git add hub/Sources/beacon-hub/UsageHistoryStore.swift
git commit -m "feat(hub): add usage-history ring buffer with 1/min sampler and Codex backfill"
```

---

# Phase 7 — TranscriptCostReader (executable: incremental background scan)

### Task 8: TranscriptCostReader

**Files:**
- Create: `hub/Sources/beacon-hub/TranscriptCostReader.swift`

Incremental, cursor-based scanner. Maintains a per-file cursor keyed by path + mtime + size; only re-reads changed files. Runs entirely on a background queue and publishes a cached `CostAggregator` snapshot. The popover-open path NEVER triggers a scan — it reads the last published breakdown.

- [ ] **Step 1: Write the reader**

```swift
import Foundation
import BeaconHubKit

// Scans Claude + Codex transcripts incrementally and keeps a CostAggregator warm in the background. The
// popover reads cached breakdowns via `breakdown(period:now:)`; it never triggers a scan synchronously.
// Per-file cursor = mtime+size: a file whose (mtime,size) is unchanged since last scan is skipped, so a
// full rescan only happens for new/changed files. The aggregator is rebuilt from scratch each scan from
// the union of parsed turns (dedup by requestId/uuid makes re-reads idempotent); Codex per-file dedup is
// implicit because we re-read a whole changed file and rebuild.
final class TranscriptCostReader {
    private let queue = DispatchQueue(label: "beacon.transcripts", qos: .utility)
    // Published aggregator snapshot, guarded by a lock so `breakdown` (MainActor) never blocks behind a
    // scan() in flight on `queue`. CostAggregator is a value type, so reads copy under the lock then
    // compute outside it. `scans` stays queue-confined (only scan() touches it).
    private let lock = NSLock()
    private var aggregator = CostAggregator(calendar: .current)
    private var timer: DispatchSourceTimer?

    // Cached, deduped turn inputs keyed by file path so a single changed file can be re-folded.
    private struct FileScan { let mtime: Date; let size: Int; let claude: [ClaudeTurn]; let codexModel: String?; let codex: [CodexTurn] }
    private var scans: [String: FileScan] = [:]

    var onUpdate: (() -> Void)?   // fired after each scan so the VM can pull a fresh breakdown.

    func start() {
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 2, repeating: 120)   // first scan shortly after launch, then every 2 min
        t.setEventHandler { [weak self] in self?.scan() }
        timer = t
        t.resume()
    }

    // Read a snapshot breakdown for the selected period. Copies the aggregator under the lock, then
    // computes outside it — never blocks behind a scan().
    func breakdown(period: CostPeriod, now: Int = Int(Date().timeIntervalSince1970)) -> CostBreakdown {
        lock.lock(); let agg = aggregator; lock.unlock()
        return agg.breakdown(period: period, now: now, table: .shared)
    }

    private func scan() {
        let home = FileManager.default.homeDirectoryForCurrentUser
        scanTree(home.appendingPathComponent(".claude/projects"), kind: .claude)
        scanTree(home.appendingPathComponent(".codex/sessions"), kind: .codex)
        rebuild()
        let cb = onUpdate
        DispatchQueue.main.async { cb?() }
    }

    private enum Kind { case claude, codex }

    private func scanTree(_ root: URL, kind: Kind) {
        guard let en = FileManager.default.enumerator(at: root,
                includingPropertiesForKeys: [.contentModificationDateKey, .fileSizeKey]) else { return }
        for case let f as URL in en where f.pathExtension == "jsonl" {
            let attrs = try? f.resourceValues(forKeys: [.contentModificationDateKey, .fileSizeKey])
            let mtime = attrs?.contentModificationDate ?? .distantPast
            let size = attrs?.fileSize ?? 0
            if let prev = scans[f.path], prev.mtime == mtime, prev.size == size { continue }   // unchanged
            guard let text = try? String(contentsOf: f, encoding: .utf8) else { continue }
            switch kind {
            case .claude: scans[f.path] = scanClaudeFile(text, mtime: mtime, size: size)
            case .codex:  scans[f.path] = scanCodexFile(text, mtime: mtime, size: size)
            }
        }
    }

    private func scanClaudeFile(_ text: String, mtime: Date, size: Int) -> FileScan {
        var turns: [ClaudeTurn] = []
        for line in text.split(separator: "\n") {
            guard let obj = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: Any],
                  let t = ClaudeTranscriptParser.parseLine(obj) else { continue }
            turns.append(t)
        }
        return FileScan(mtime: mtime, size: size, claude: turns, codexModel: nil, codex: [])
    }

    private func scanCodexFile(_ text: String, mtime: Date, size: Int) -> FileScan {
        var model: String?
        var turns: [CodexTurn] = []
        for line in text.split(separator: "\n") {
            guard let obj = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: Any] else { continue }
            if model == nil, let m = CodexTranscriptParser.parseModel(obj) { model = m }
            if let t = CodexTranscriptParser.parseLine(obj) { turns.append(t) }
        }
        return FileScan(mtime: mtime, size: size, claude: [], codexModel: model, codex: turns)
    }

    // Rebuild the aggregator from all cached file scans. Claude dedup by requestId/uuid handles any
    // overlap from resumed sessions; Codex turns are whole-file re-reads so there is no cross-file dup.
    private func rebuild() {
        var agg = CostAggregator(calendar: .current)
        for (_, scan) in scans {
            for t in scan.claude { agg.addClaude(t) }
            if let model = scan.codexModel {
                for t in scan.codex { agg.addCodex(model: model, turn: t) }
            }
        }
        lock.lock(); aggregator = agg; lock.unlock()   // publish atomically for breakdown()
    }
}
```

- [ ] **Step 2: Verify compile**

Run: `cd hub && swift build`
Expected: Builds with no errors.

- [ ] **Step 3: Commit**

```bash
git add hub/Sources/beacon-hub/TranscriptCostReader.swift
git commit -m "feat(hub): add incremental background transcript cost reader"
```

---

# Phase 8 — Statusline cost field (executable edit)

### Task 9: Parse `cost.total_cost_usd`

**Files:**
- Modify: `hub/Sources/beacon-hub/ClaudeCodeBridge.swift:441-461`

The `cost` object (`total_cost_usd`, `total_duration_ms`, `total_lines_added/removed`) is documented statusline input since CC v2.1.90. Parse it as a per-session cross-check; absence must not affect the existing `context_window`/`rate_limits` handling. This is a small, low-risk add. It surfaces a value on the bridge that AppDelegate can later forward to the VM as the active-session live cost (used as the "today" cross-check footnote source if desired); for v1 it is parsed and exposed via a callback, kept minimal.

- [ ] **Step 1: Add a callback property near the other bridge callbacks**

Find the existing callback declarations (e.g. `onClaudeUsage`) at the top of `ClaudeCodeBridge` and add alongside them:

```swift
    // Per-session live cost from the statusline `cost` object (CC >= 2.1.90). Optional cross-check only;
    // never gates the context_window/rate_limits path. nil-safe: absent field => callback not fired.
    var onSessionCost: ((_ sessionId: String?, _ totalUSD: Double) -> Void)?
```

- [ ] **Step 2: Parse the cost object inside `handleStatusline`**

In `handleStatusline(_:)`, after the `context_window` block closes (current line 451, the `}` ending the `if let cw` block) and before the `rate_limits` block (line 454), insert:

```swift
        // Optional `cost` object (CC >= 2.1.90): total_cost_usd is a per-session cross-check. Guarded so a
        // missing field leaves context_window/rate_limits handling untouched.
        if let cost = body["cost"] as? [String: Any], let usd = Self.double(cost["total_cost_usd"]) {
            let cb = onSessionCost
            DispatchQueue.main.async { cb?(sid, usd) }
        }
```

- [ ] **Step 3: Verify compile**

Run: `cd hub && swift build`
Expected: Builds with no errors.

- [ ] **Step 4: Commit**

```bash
git add hub/Sources/beacon-hub/ClaudeCodeBridge.swift
git commit -m "feat(hub): parse statusline cost.total_cost_usd as a per-session cross-check"
```

---

# Phase 9 — View model state (executable edit)

### Task 10: HubViewModel — tab/period + trends payloads

**Files:**
- Modify: `hub/Sources/beacon-hub/HubViewModel.swift`

Add the Trends UI state: selected tab and chart period (both persisted to UserDefaults), plus the published payloads the Trends view renders (chart series source samples, cost breakdown, pace rows). Keep the existing `Now`-tab fields untouched.

- [ ] **Step 1: Add tab/period enums and published state**

Add these types above the class (after the imports), and the published fields + persistence inside the class. Insert the enums before `final class HubViewModel`:

```swift
// Selected top-level view. Persisted so the popover reopens where the user left it.
enum HubTab: String { case now, trends }
// Chart/cost period. 6h/24h read the 5h-window pct; 7d reads the 7d-window pct (issue #57).
enum TrendsPeriod: String, CaseIterable { case h6, h24, d7
    var label: String { switch self { case .h6: return "6h"; case .h24: return "24h"; case .d7: return "7d" } }
}
```

Then inside `HubViewModel`, after the existing `@Published var now: Date` (line 19), add:

```swift
    // --- Trends (issue #57). Persisted selections + cached payloads published by AppDelegate. ---
    @Published var tab: HubTab { didSet { UserDefaults.standard.set(tab.rawValue, forKey: Self.tabKey) } }
    @Published var period: TrendsPeriod { didSet { UserDefaults.standard.set(period.rawValue, forKey: Self.periodKey) } }
    @Published var historySamples: [UsageSample] = []      // raw ring-buffer samples for the chart
    @Published var cost: CostBreakdown = CostBreakdown(rows: [], totalUSD: 0)
    @Published var claudePace: BurnPaceResult = BurnPaceResult(pace: nil, capEpoch: nil, style: nil)
    @Published var codexPace: BurnPaceResult = BurnPaceResult(pace: nil, capEpoch: nil, style: nil)

    static let tabKey = "BeaconTrendsTab"
    static let periodKey = "BeaconTrendsPeriod"
```

- [ ] **Step 2: Seed tab/period from UserDefaults in `init`**

Replace the existing initializer (lines 32-35):

```swift
    init(now: Date = Date(), muted: Bool = UserDefaults.standard.bool(forKey: "BeaconPromptSoundMuted")) {
        self.now = now
        self.muted = muted
    }
```

with:

```swift
    init(now: Date = Date(), muted: Bool = UserDefaults.standard.bool(forKey: "BeaconPromptSoundMuted")) {
        self.now = now
        self.muted = muted
        self.tab = UserDefaults.standard.string(forKey: Self.tabKey).flatMap(HubTab.init) ?? .now
        self.period = UserDefaults.standard.string(forKey: Self.periodKey).flatMap(TrendsPeriod.init) ?? .h24
    }
```

- [ ] **Step 3: Verify compile**

Run: `cd hub && swift build`
Expected: Builds with no errors.

- [ ] **Step 4: Commit**

```bash
git add hub/Sources/beacon-hub/HubViewModel.swift
git commit -m "feat(hub): add persisted tab/period and trends payloads to HubViewModel"
```

---

# Phase 10 — Trends UI (executable: new SwiftUI + HubPanel edit)

### Task 11: UsageChart (Swift Charts dual-line with gaps + cap line)

**Files:**
- Create: `hub/Sources/beacon-hub/UsageChart.swift`

- [ ] **Step 1: Write the chart view**

```swift
import SwiftUI
import Charts
import BeaconHubKit

// Dual-line chart of 5h-window pct (6h/24h) or 7d-window pct (7d) for Claude + Codex. Gaps render as
// line breaks (each ChartSeries segment is its own LineMark series), with a dashed red cap rule at 100%.
// macOS 13 ships Swift Charts, which is the package's deployment target (Package.swift), so no Path fallback.
struct UsageChart: View {
    let claude: ChartSeries
    let codex: ChartSeries
    let range: ClosedRange<Int>
    let axisLabels: [String]   // exactly 3: start / mid / now

    var body: some View {
        VStack(spacing: 2) {
            Chart {
                segmentMarks(claude, color: .blue, name: "Claude")
                segmentMarks(codex, color: .purple, name: "Codex")
                RuleMark(y: .value("Cap", 100))
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [3, 3]))
                    .foregroundStyle(.red.opacity(0.4))
            }
            .chartYScale(domain: 0...100)
            .chartXScale(domain: range.lowerBound...range.upperBound)
            .chartXAxis(.hidden)
            .chartYAxis(.hidden)
            .frame(height: 64)
            HStack {
                ForEach(Array(axisLabels.enumerated()), id: \.offset) { i, label in
                    Text(label).font(.system(size: 9)).foregroundStyle(.tertiary)
                    if i < axisLabels.count - 1 { Spacer() }
                }
            }
        }
    }

    @ChartContentBuilder
    private func segmentMarks(_ series: ChartSeries, color: Color, name: String) -> some ChartContent {
        ForEach(Array(series.segments.enumerated()), id: \.offset) { segIdx, segment in
            ForEach(segment, id: \.ts) { point in
                LineMark(
                    x: .value("t", point.ts),
                    y: .value("pct", point.pct),
                    series: .value("seg", "\(name)-\(segIdx)")   // distinct series => break between segments
                )
                .foregroundStyle(color)
                .lineStyle(StrokeStyle(lineWidth: 1.5))
            }
        }
    }
}
```

- [ ] **Step 2: Verify compile**

Run: `cd hub && swift build`
Expected: Builds with no errors.

- [ ] **Step 3: Commit**

```bash
git add hub/Sources/beacon-hub/UsageChart.swift
git commit -m "feat(hub): add Swift Charts dual-line usage chart with gap breaks"
```

---

### Task 12: TrendsView (chart card + pace rows + cost card)

**Files:**
- Create: `hub/Sources/beacon-hub/TrendsView.swift`

Mirrors the approved Variant 1 mockup (`docs/mockups/hub-trends-tab-options.html`): a chart card with a `6h | 24h | 7d` mini selector, pinned pace rows, then a cost card scoped to the selected period. Reuses the existing `Module` chrome (defined in `DeckUI.swift`).

- [ ] **Step 1: Write the Trends view**

```swift
import SwiftUI
import BeaconHubKit

struct TrendsView: View {
    @ObservedObject var model: HubViewModel

    var body: some View {
        VStack(spacing: 10) {
            chartCard
            costCard
        }
    }

    // --- chart card: header label + mini period selector, chart, legend, pace rows ---
    private var chartCard: some View {
        Module {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Text(model.period == .d7 ? "7D WINDOW" : "5H WINDOW")
                        .font(.system(size: 11, weight: .bold)).foregroundStyle(.secondary)
                        .tracking(0.5)
                    Spacer()
                    MiniPeriodPicker(period: $model.period)
                }
                UsageChart(claude: claudeSeries, codex: codexSeries, range: chartRange, axisLabels: axisLabels)
                HStack(spacing: 14) {
                    Legend(color: .blue, text: "Claude \(legendPct(model.usage.claude))")
                    Legend(color: .purple, text: "Codex \(legendPct(model.usage.codex))")
                }
                if claudeSeries.segments.isEmpty && codexSeries.segments.isEmpty {
                    Text("line breaks = Hub not running; history starts at install")
                        .font(.system(size: 9)).italic().foregroundStyle(.tertiary)
                }
                PaceRow(name: "Claude", pace: model.claudePace, now: model.now)
                PaceRow(name: "Codex", pace: model.codexPace, now: model.now)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    // --- cost card: total + per-model rows + footnote ---
    private var costCard: some View {
        Module {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text(costScopeLabel).font(.system(size: 11, weight: .bold)).foregroundStyle(.secondary).tracking(0.5)
                    Spacer()
                }
                HStack {
                    Text("Total").font(.system(size: 12)).foregroundStyle(.secondary)
                    Spacer()
                    Text(String(format: "$%.2f est.", model.cost.totalUSD)).font(.system(size: 12, weight: .semibold))
                }
                ForEach(model.cost.rows, id: \.model) { row in
                    CostRow(row: row, maxCost: maxRowCost)
                }
                Text("est. from transcripts at API list prices - plan is flat-rate")
                    .font(.system(size: 9)).foregroundStyle(.tertiary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    // --- derived ---
    private var chartMetric: ChartMetric { model.period == .d7 ? .d7 : .h5 }
    private var chartRange: ClosedRange<Int> {
        let now = Int(model.now.timeIntervalSince1970)
        let span: Int = { switch model.period { case .h6: return 6*3600; case .h24: return 24*3600; case .d7: return 7*86_400 } }()
        return (now - span)...now
    }
    private var claudeSeries: ChartSeries {
        ChartSeries.build(samples: model.historySamples, provider: .claude, metric: chartMetric, range: chartRange, breakSeconds: 600)
    }
    private var codexSeries: ChartSeries {
        ChartSeries.build(samples: model.historySamples, provider: .codex, metric: chartMetric, range: chartRange, breakSeconds: 600)
    }
    private var axisLabels: [String] {
        let f = DateFormatter()
        switch model.period {
        case .h6, .h24: f.dateFormat = "h a"
        case .d7: f.dateFormat = "MMM d"
        }
        let lo = Date(timeIntervalSince1970: TimeInterval(chartRange.lowerBound))
        let mid = Date(timeIntervalSince1970: TimeInterval((chartRange.lowerBound + chartRange.upperBound) / 2))
        return [f.string(from: lo), f.string(from: mid), "now"]
    }
    private var maxRowCost: Double { model.cost.rows.compactMap { $0.costUSD }.max() ?? 1 }
    private var costScopeLabel: String {
        switch model.period { case .h6: return "COST - LAST 6H"; case .h24: return "COST - TODAY"; case .d7: return "COST - THIS WEEK" }
    }
    private func legendPct(_ p: ProviderUsage) -> String {
        let w = model.period == .d7 ? p.d7 : p.h5
        return w.pct.map { "\($0)%" } ?? "--"
    }
}

private struct MiniPeriodPicker: View {
    @Binding var period: TrendsPeriod
    var body: some View {
        HStack(spacing: 0) {
            ForEach(TrendsPeriod.allCases, id: \.self) { p in
                Text(p.label)
                    .font(.system(size: 10, weight: .semibold))
                    .padding(.vertical, 3).frame(width: 40)
                    .background(period == p ? Color.primary.opacity(0.14) : .clear, in: RoundedRectangle(cornerRadius: 6))
                    .foregroundStyle(period == p ? .primary : .secondary)
                    .contentShape(Rectangle())
                    .onTapGesture { period = p }
            }
        }
        .padding(2)
        .background(Color.primary.opacity(0.06), in: RoundedRectangle(cornerRadius: 8))
    }
}

private struct Legend: View {
    let color: Color; let text: String
    var body: some View {
        HStack(spacing: 5) {
            RoundedRectangle(cornerRadius: 2).fill(color).frame(width: 8, height: 8)
            Text(text).font(.system(size: 10)).foregroundStyle(.secondary)
        }
    }
}

private struct PaceRow: View {
    let name: String; let pace: BurnPaceResult; let now: Date
    var body: some View {
        if let p = pace.pace, let cap = pace.capEpoch, let style = pace.style {
            HStack {
                Text("\(name) pace").font(.system(size: 12)).foregroundStyle(.secondary)
                Spacer()
                Text(text(p, cap, style)).font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(style == .warn ? .orange : .green)
            }
        }
    }
    private func text(_ pace: Double, _ cap: Int, _ style: BurnPaceResult.Style) -> String {
        if style == .safe { return String(format: "+%.0f%%/h - safe past reset", pace) }
        let f = DateFormatter(); f.timeStyle = .short
        return String(format: "+%.0f%%/h - caps ~%@", pace, f.string(from: Date(timeIntervalSince1970: TimeInterval(cap))))
    }
}

private struct CostRow: View {
    let row: ModelCostRow; let maxCost: Double
    var body: some View {
        HStack(spacing: 8) {
            Text(displayName).font(.system(size: 11, weight: .semibold)).frame(width: 86, alignment: .leading)
            Text(tokens).font(.system(size: 11)).foregroundStyle(.secondary).frame(width: 64, alignment: .leading)
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    Capsule().fill(.primary.opacity(0.10))
                    Capsule().fill(barColor).frame(width: max(2, geo.size.width * fraction))
                }
            }.frame(height: 4)
            Text(costText).font(.system(size: 11, weight: .semibold)).frame(width: 50, alignment: .trailing)
        }
    }
    private var fraction: Double { guard let c = row.costUSD, maxCost > 0 else { return 0 }; return min(1, c / maxCost) }
    private var barColor: Color { row.model.hasPrefix("gpt") ? .purple : .blue }
    private var costText: String { row.costUSD.map { String(format: "$%.2f", $0) } ?? "-" }
    private var tokens: String {
        let m = Double(row.tokens) / 1_000_000
        return m >= 1 ? String(format: "%.1fM tok", m) : String(format: "%.0fK tok", Double(row.tokens) / 1000)
    }
    private var displayName: String {
        switch row.model {
        case "claude-opus-4-8": return "Opus 4.8"
        case "claude-sonnet-4-6", "sonnet": return "Sonnet 4.6"
        case "claude-haiku-4-5-20251001", "haiku": return "Haiku 4.5"
        case "claude-fable-5", "fable": return "Fable 5"
        case "gpt-5.5": return "GPT-5.5"
        default: return row.model
        }
    }
}
```

- [ ] **Step 2: Verify compile**

Run: `cd hub && swift build`
Expected: Builds with no errors. (If `Module` is `private` in `DeckUI.swift`, change its access to internal — see Task 13 note.)

- [ ] **Step 3: Commit**

```bash
git add hub/Sources/beacon-hub/TrendsView.swift
git commit -m "feat(hub): add Trends tab view (chart card, pace rows, cost card)"
```

---

### Task 13: HubPanel — Now | Trends segmented control

**Files:**
- Modify: `hub/Sources/beacon-hub/HubPanel.swift:12-38`
- Possibly modify: `hub/Sources/beacon-hub/DeckUI.swift` (relax `Module` access if it is `private`/`fileprivate`)

The `Now` tab must be pixel-identical to the current panel. Extract the current body content into a `NowTab` subview, add the segmented control, and switch between `NowTab` and `TrendsView`. The header and action bar stay outside the tab so they show on both (matching the mockup, where header + action bar bracket the tab content). Per the mockup, the segmented control sits directly under the header.

- [ ] **Step 1: Confirm `Module` is reachable from new files**

`TrendsView`/`UsageChart` reference `Module`. Open `DeckUI.swift` and check the `Module` declaration. If it is declared `private struct Module` or `fileprivate`, change it to `struct Module` (internal) so the new files in the same target can use it. If it is already internal, no change.

Run: `cd hub && grep -n "struct Module" hub/Sources/beacon-hub/DeckUI.swift`

- [ ] **Step 2: Restructure `HubPanel.body`**

Replace the body (lines 12-38):

```swift
    var body: some View {
        VStack(spacing: 10) {
            if let banner = model.bridgeAlert ?? model.alert.map({ "\($0) — couldn't show prompt" }) {
                Banner(text: banner)
            }
            HeaderModule(model: model, closeAndRun: closeAndRun)
            if !model.errors.isEmpty {
                Module {
                    VStack(alignment: .leading, spacing: 4) {
                        ForEach(model.errors, id: \.self) { Label($0, systemImage: "exclamationmark.triangle.fill") }
                    }
                    .font(.system(size: 11))
                    .foregroundStyle(.red)
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            HStack(spacing: 10) {
                ProviderCard(name: "Claude", usage: model.usage.claude, now: model.now)
                ProviderCard(name: "Codex", usage: model.usage.codex, now: model.now)
            }
            TogglesModule(model: model)
            ActionBar(model: model, closeAndRun: closeAndRun)
        }
        .padding(12)
        .frame(width: 340)
    }
```

with:

```swift
    var body: some View {
        VStack(spacing: 10) {
            if let banner = model.bridgeAlert ?? model.alert.map({ "\($0) — couldn't show prompt" }) {
                Banner(text: banner)
            }
            HeaderModule(model: model, closeAndRun: closeAndRun)
            TabSwitcher(tab: $model.tab)
            switch model.tab {
            case .now:    NowTab(model: model)
            case .trends: TrendsView(model: model)
            }
            ActionBar(model: model, closeAndRun: closeAndRun)
        }
        .padding(12)
        .frame(width: 340)
    }
```

- [ ] **Step 3: Add `NowTab` and `TabSwitcher` subviews**

Insert after the `HubPanel` struct closing brace (before `// MARK: - Header`, line 40). `NowTab` holds the exact former content (errors module, provider cards, toggles) so the Now tab stays pixel-identical:

```swift
// The unchanged "Now" surface: errors, the two provider cards, and the toggles. Extracted verbatim from
// the pre-Trends panel so the Now tab is pixel-identical to the original layout.
private struct NowTab: View {
    @ObservedObject var model: HubViewModel
    var body: some View {
        VStack(spacing: 10) {
            if !model.errors.isEmpty {
                Module {
                    VStack(alignment: .leading, spacing: 4) {
                        ForEach(model.errors, id: \.self) { Label($0, systemImage: "exclamationmark.triangle.fill") }
                    }
                    .font(.system(size: 11))
                    .foregroundStyle(.red)
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            HStack(spacing: 10) {
                ProviderCard(name: "Claude", usage: model.usage.claude, now: model.now)
                ProviderCard(name: "Codex", usage: model.usage.codex, now: model.now)
            }
            TogglesModule(model: model)
        }
    }
}

// Top-level Now | Trends segmented control (mockup Option B). Tap-driven; persistence lives in the VM.
private struct TabSwitcher: View {
    @Binding var tab: HubTab
    var body: some View {
        HStack(spacing: 0) {
            segment("Now", .now)
            segment("Trends", .trends)
        }
        .padding(2)
        .background(Color.primary.opacity(0.06), in: RoundedRectangle(cornerRadius: 8))
    }
    private func segment(_ title: String, _ value: HubTab) -> some View {
        Text(title)
            .font(.system(size: 11, weight: .semibold))
            .frame(maxWidth: .infinity).padding(.vertical, 4)
            .background(tab == value ? Color.primary.opacity(0.14) : .clear, in: RoundedRectangle(cornerRadius: 6))
            .foregroundStyle(tab == value ? .primary : .secondary)
            .contentShape(Rectangle())
            .onTapGesture { tab = value }
    }
}
```

- [ ] **Step 4: Update the DEBUG preview to exercise both tabs**

In the `#Preview` (lines 328-340), after `m.usage = …`, add seed data so the Trends tab renders in previews:

```swift
    m.tab = .trends
    m.historySamples = (0..<60).map { i in
        UsageSample(ts: 1_733_800_000 - (60 - i) * 60, provider: .claude, h5: i, d7: i / 8)
    } + (0..<60).map { i in
        UsageSample(ts: 1_733_800_000 - (60 - i) * 60, provider: .codex, h5: i / 3, d7: 50 + i / 12)
    }
    m.cost = CostBreakdown(rows: [
        ModelCostRow(model: "claude-opus-4-8", tokens: 1_200_000, costUSD: 14.10),
        ModelCostRow(model: "gpt-5.5", tokens: 1_800_000, costUSD: 2.50),
    ], totalUSD: 16.60)
```

- [ ] **Step 5: Verify compile + build**

Run: `cd hub && swift build`
Expected: Builds with no errors.

- [ ] **Step 6: Commit**

```bash
git add hub/Sources/beacon-hub/HubPanel.swift hub/Sources/beacon-hub/DeckUI.swift
git commit -m "feat(hub): add Now|Trends segmented control to the popover"
```

---

# Phase 11 — Wiring (executable: AppDelegate + MenubarController)

### Task 14: MenubarController — persistence passthrough + trends setters

**Files:**
- Modify: `hub/Sources/beacon-hub/MenubarController.swift`

`MenubarController` owns the `HubViewModel`. Add setters AppDelegate calls to publish the history samples, cost breakdown, and pace results onto the VM. Tab/period persistence already lives in the VM (Task 10) via `didSet`, so no extra work there — the VM's `@Published` bindings drive the segmented controls directly.

- [ ] **Step 1: Add trends setters**

After `setUsage(_:errors:)` (ends line 142), add:

```swift
    // --- Trends payloads (issue #57). AppDelegate pushes cached values computed off the UI path. ---
    func setHistory(_ samples: [UsageSample]) { model.historySamples = samples }
    func setCost(_ cost: CostBreakdown) { model.cost = cost }
    func setPace(claude: BurnPaceResult, codex: BurnPaceResult) {
        model.claudePace = claude
        model.codexPace = codex
    }
```

- [ ] **Step 2: Verify compile**

Run: `cd hub && swift build`
Expected: Builds with no errors.

- [ ] **Step 3: Commit**

```bash
git add hub/Sources/beacon-hub/MenubarController.swift
git commit -m "feat(hub): add trends payload setters to MenubarController"
```

---

### Task 15: AppDelegate — instantiate subsystems and feed the VM

**Files:**
- Modify: `hub/Sources/beacon-hub/AppDelegate.swift`

Own the store and reader, run the sampler off the existing poll callback, drive burn-pace estimators from each new merged usage, recompute the cost breakdown when the reader updates or the period changes, and push everything to the menubar. Nothing here runs a synchronous scan on popover open.

- [ ] **Step 1: Add properties**

After `private let poller = UsagePoller()` (line 14), add:

```swift
    private let history = UsageHistoryStore()
    private let costReader = TranscriptCostReader()
    private var claudePace = BurnPaceEstimator()
    private var codexPace = BurnPaceEstimator()
```

- [ ] **Step 2: Start the subsystems in `applicationDidFinishLaunching`**

After `startPoller()` (line 34), add:

```swift
        startTrends()
```

Then add the `startTrends` method (place it near `startPoller`, after line 231):

```swift
    private func startTrends() {
        history.start()
        costReader.onUpdate = { [weak self] in self?.refreshCost() }
        costReader.start()
        // Republish the cost breakdown when the period selection changes (observe the VM's published period).
        menubar.onPeriodChanged = { [weak self] in self?.refreshCost() }
    }

    private func refreshCost() {
        let period = menubar.currentCostPeriod()
        let bd = costReader.breakdown(period: period)
        Task { @MainActor in self.menubar.setCost(bd) }
    }
```

- [ ] **Step 3: Hook the sampler + pace into the usage path**

In `rebuildUsage()` (lines 246-254), after `menubar.setUsage(usage, errors: errors)` (line 252), add:

```swift
        history.record(usage)
        history.snapshot { [weak self] samples in self?.menubar.setHistory(samples) }
        updatePace(usage)
```

Then add `updatePace` after `rebuildUsage`:

```swift
    // Feed the 5h-window pct of each provider into its EMA estimator and publish the pace rows.
    private func updatePace(_ usage: Usage) {
        let now = Int(Date().timeIntervalSince1970)
        let c = usage.claude.h5
        let x = usage.codex.h5
        let cPace = c.pct.map { claudePace.update(pct: $0, reset: c.reset, now: now) }
            ?? BurnPaceResult(pace: nil, capEpoch: nil, style: nil)
        let xPace = x.pct.map { codexPace.update(pct: $0, reset: x.reset, now: now) }
            ?? BurnPaceResult(pace: nil, capEpoch: nil, style: nil)
        menubar.setPace(claude: cPace, codex: xPace)
    }
```

- [ ] **Step 4: Add the period bridge to MenubarController**

The cost period depends on the VM's selected `period`, which lives in the VM owned by MenubarController. Add to `MenubarController` (Task 14 file) a small bridge:

```swift
    var onPeriodChanged: (() -> Void)?
    func currentCostPeriod() -> CostPeriod {
        switch model.period { case .h6: return .last6h; case .h24: return .day; case .d7: return .week }
    }
```

And make the VM notify on period change. In `MenubarController.wireModel()` (after line 72), add an observation. Simplest: have the VM expose an `onPeriodChange` closure (add `var onPeriodChange: () -> Void = {}` to the VM and call it in `period`'s `didSet`), then wire it:

In `HubViewModel`, change the `period` `didSet` (Task 10) to also fire a closure:

```swift
    var onPeriodChange: () -> Void = {}
    @Published var period: TrendsPeriod {
        didSet {
            UserDefaults.standard.set(period.rawValue, forKey: Self.periodKey)
            onPeriodChange()
        }
    }
```

In `MenubarController.wireModel()` add:

```swift
        model.onPeriodChange = { [weak self] in self?.onPeriodChanged?() }
```

- [ ] **Step 5: Verify compile + full build**

Run: `cd hub && swift build`
Expected: Builds with no errors.

- [ ] **Step 6: Commit**

```bash
git add hub/Sources/beacon-hub/AppDelegate.swift hub/Sources/beacon-hub/MenubarController.swift hub/Sources/beacon-hub/HubViewModel.swift
git commit -m "feat(hub): wire history sampler, cost reader, and burn pace into the app"
```

---

# Phase 12 — Verification

### Task 16: Full test + build + manual smoke

**Files:** none (verification only).

- [ ] **Step 1: Run the full unit suite**

Run: `cd hub && swift test`
Expected: All `BeaconHubKitTests` pass (PricingTests, CostMathTests, TranscriptParsingTests, CostAggregatorTests, BurnPaceTests, UsageHistoryTests, plus the pre-existing suites).

- [ ] **Step 2: Build the app bundle**

Run: `cd hub && ./build-app.sh`
Expected: Produces `Beacon Hub.app` with no errors.

- [ ] **Step 3: Manual smoke (requires the signed bundle for BLE; the panel renders without a device)**

Run: `"./hub/Beacon Hub.app/Contents/MacOS/beacon-hub"` and open the menubar popover.
Verify against the acceptance criteria:
- `Now | Trends` control present; **Now tab pixel-identical** to the previous panel (header, two provider cards with 5h/7d bars + reset hints, toggles, action bar).
- Trends tab: `6h | 24h | 7d` mini selector switches the chart metric (5h-pct for 6h/24h, 7d-pct for 7d) and the cost-card scope together; the `COST - …` label and total update with it.
- Cost card shows per-model rows + total; `est.` labeling and the transcript/list-price footnote present. (`<synthetic>` excluded; a resumed session must not double-count — verified by `CostAggregatorTests.testDedupByKey`.)
- Pace rows hidden until ≥3 movement samples; warn/safe coloring once present.
- Popover open is instant (no synchronous transcript scan — the reader runs on its 2-min background timer; the cost breakdown is cached).
- Quit and reopen: the selected tab + period persist (UserDefaults).
- On first run with the history file absent, the Codex chart line shows backfilled history immediately; the Claude line starts forward-only.

- [ ] **Step 4: Confirm the contract is untouched**

Run: `cd hub && git diff --stat main -- Sources/BeaconHubKit/Protocol.swift CONTRACT.md`
Expected: **No changes** to `Protocol.swift` or `CONTRACT.md` — the `"v":1` BLE wire format and firmware are frozen (out of scope).

- [ ] **Step 5: Final commit (if any verification fixups were needed) and open PR**

```bash
git push -u origin <branch>
gh pr create --title "feat(hub): Usage Trends tab — charts, burn pace, per-model cost (menubar-only)" --body "Implements #57. Menubar-only; BLE \"v\":1 contract and firmware untouched."
```

---

## Self-review against the spec

- **Now | Trends control, Now pixel-identical** — Task 13 extracts the original content verbatim into `NowTab`; header/action bar bracket both tabs.
- **Period selector switches chart + cost together** — `TrendsView` derives `chartMetric`, `chartRange`, and `costScopeLabel` from `model.period`; AppDelegate.`refreshCost` re-queries on period change.
- **History sampled ~1/min, survives restart, pruned at 14d, gaps as breaks** — `UsageHistoryStore` (Task 7) + `ChartSeries` segment breaks (Task 6).
- **Codex backfill on first run (file absent)** — `UsageHistoryStore.backfillCodex` (Task 7), gated on `!existed`.
- **Cost card per-model + total, `<synthetic>` excluded, dedup verified, `est.` labeling** — Tasks 1-4 + 12; `<synthetic>` skip in `ClaudeTranscriptParser`, dedup in `CostAggregator`, `est.`/footnote in `TrendsView`.
- **Statusline `cost.total_cost_usd` parsed, absence harmless** — Task 9, guarded insert.
- **Burn pace rows with cap-vs-reset coloring, hidden until enough samples, sane across reset** — Task 5 + `PaceRow` (Task 12).
- **Popover open instant** — reader scans on a 2-min background timer; `breakdown` reads the cached aggregator; no scan on open.
- **Tab + period persisted** — VM `didSet` → UserDefaults (Task 10), seeded in `init`.
- **Unit tests (table-driven) for pricing/cache tiers, dedup, EMA/pace incl. reset, period bucketing across midnight, fixtures for both transcript formats** — Tasks 1-6.

**Out of scope (not implemented, per issue):** device/firmware/BLE changes; month views/heatmaps/tooltips/CSV/per-project; Opus weekly bucket; freshness indicator.
