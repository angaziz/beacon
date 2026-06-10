import Foundation
import BeaconHubKit

// Scans Claude + Codex transcripts incrementally and keeps a CostAggregator warm in the background. The
// popover reads cached breakdowns via `breakdown(period:now:)`; it never triggers a scan synchronously.
// Per-file cursor = mtime+size: a file whose (mtime,size) is unchanged since last scan is skipped, so a
// full rescan only happens for new/changed files. The aggregator is rebuilt from the union of parsed
// turns each scan (Claude dedup by requestId/uuid makes re-reads idempotent; Codex is whole-file re-read).
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
        t.schedule(deadline: .now() + 0.5, repeating: 120)   // first scan right after launch, then every 2 min
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

    private enum Kind { case claude, codex }

    private func scan() {
        let home = FileManager.default.homeDirectoryForCurrentUser
        scanTree(home.appendingPathComponent(".claude/projects"), kind: .claude)
        scanTree(home.appendingPathComponent(".codex/sessions"), kind: .codex)
        rebuild()
        let cb = onUpdate
        DispatchQueue.main.async { cb?() }
    }

    private func scanTree(_ root: URL, kind: Kind) {
        guard let en = FileManager.default.enumerator(at: root,
                includingPropertiesForKeys: [.contentModificationDateKey, .fileSizeKey]) else { return }
        // Cost periods top out at "this week", so a file untouched for >8 days can't contribute. Skipping
        // it at the stat (no read/parse) keeps the first scan fast on a large corpus (thousands of files).
        let cutoff = Date(timeIntervalSinceNow: -8 * 86_400)
        for case let f as URL in en where f.pathExtension == "jsonl" {
            let attrs = try? f.resourceValues(forKeys: [.contentModificationDateKey, .fileSizeKey])
            let mtime = attrs?.contentModificationDate ?? .distantPast
            let size = attrs?.fileSize ?? 0
            if mtime < cutoff { continue }   // outside any cost period; never read it
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

    // Rebuild the aggregator from all cached file scans. Claude dedup by requestId/uuid handles overlap
    // from resumed sessions; Codex turns are whole-file re-reads so there is no cross-file dup.
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
