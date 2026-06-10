import Foundation
import BeaconHubKit

// Append-only ring buffer of UsageSample under ~/.beacon/usage-history.jsonl, sampled ~1/min from the
// existing poll pipeline. Retention 14 days; pruned on launch and occasionally on write. On first run
// (file absent) the Codex line is backfilled from existing ~/.codex/sessions rate_limits; Claude is
// forward-only. All disk IO runs on a private queue; loaded samples are cached in memory for chart reads.
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

    // Returns a snapshot for the chart (completion fires on the main thread).
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
        if Int.random(in: 0..<60) == 0 { prune() }   // occasional rollover prune
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
        rewrite(kept)
    }

    private func rewrite(_ all: [UsageSample]) {
        let enc = JSONEncoder()
        var blob = Data()
        for s in all { if let d = try? enc.encode(s) { blob.append(d); blob.append(0x0A) } }
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
        samples = (samples + seeded).sorted { $0.ts < $1.ts }
        for s in samples where s.provider == .codex { lastSampleTs[.codex] = max(lastSampleTs[.codex] ?? 0, s.ts) }
        rewrite(samples)
    }
}
