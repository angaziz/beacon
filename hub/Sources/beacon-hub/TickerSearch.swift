import Foundation
import BeaconHubKit

// Thin network layer for ticker discovery (issue #92 B2). Mirrors UsagePoller's URLSession style
// (ephemeral config, ~10s request timeout, dataTask(...).resume(), manual JSON via the pure mappers).
// All classification/ordering lives in BeaconHubKit (TickerCatalog) and is host-tested; this layer just
// fetches bytes and hands them off, so it is deliberately NOT unit-tested (no network in CI).
//
// Binance exchangeInfo is a large, slow-changing universe: fetch once and cache the RAW JSON to
// Application Support with a ~daily TTL (re-fetch only if missing or older than 24h). Yahoo search is
// live per query (debouncing is the UI's job in B4). Completions fire on an arbitrary URLSession queue;
// callers hop to main as needed.
final class TickerSearch {
    private let session: URLSession
    private let cacheTTL: TimeInterval = 24 * 60 * 60

    init() {
        let cfg = URLSessionConfiguration.ephemeral
        cfg.timeoutIntervalForRequest = 10
        self.session = URLSession(configuration: cfg)
    }

    // Binance universe. Loads a fresh-enough cache if present; otherwise GETs exchangeInfo, caches the raw
    // body, and maps it. On network failure falls back to a stale cache (any age) when one exists, else [].
    func fetchBinanceCatalog(completion: @escaping ([TickerCandidate]) -> Void) {
        if let cached = loadCache(fresh: true) {
            completion(BinanceCatalog.map(exchangeInfoJSON: cached))
            return
        }
        guard let url = URL(string: "https://data-api.binance.vision/api/v3/exchangeInfo") else {
            completion(staleFallback()); return
        }
        session.dataTask(with: url) { [weak self] data, resp, _ in
            guard let self else { completion([]); return }
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            guard code == 200, let data, !data.isEmpty else {
                completion(self.staleFallback()); return
            }
            self.writeCache(data)
            completion(BinanceCatalog.map(exchangeInfoJSON: data))
        }.resume()
    }

    // Live Yahoo search. URL-escapes the query, sends a browser-ish UA (Yahoo 4xx's headerless calls,
    // matching UsagePoller's Yahoo handling), maps via YahooCatalog. [] on any failure.
    func searchYahoo(_ query: String, completion: @escaping ([TickerCandidate]) -> Void) {
        let trimmed = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty,
              let q = trimmed.addingPercentEncoding(withAllowedCharacters: .urlQueryValueAllowed),
              let url = URL(string: "https://query1.finance.yahoo.com/v1/finance/search?q=\(q)")
        else { completion([]); return }

        var req = URLRequest(url: url)
        req.setValue("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko)",
                     forHTTPHeaderField: "User-Agent")
        req.setValue("application/json", forHTTPHeaderField: "Accept")

        session.dataTask(with: req) { data, resp, _ in
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            guard code == 200, let data else { completion([]); return }
            completion(YahooCatalog.map(searchJSON: data))
        }.resume()
    }

    // --- raw cache (Application Support/Beacon/binance-exchangeInfo.json) ---

    private var cacheURL: URL? {
        guard let dir = FileManager.default.urls(for: .applicationSupportDirectory,
                                                 in: .userDomainMask).first else { return nil }
        return dir.appendingPathComponent("Beacon/binance-exchangeInfo.json")
    }

    // fresh=true returns the cache only if newer than cacheTTL; fresh=false returns it at any age.
    private func loadCache(fresh: Bool) -> Data? {
        guard let url = cacheURL,
              let attrs = try? FileManager.default.attributesOfItem(atPath: url.path),
              let modified = attrs[.modificationDate] as? Date
        else { return nil }
        if fresh && Date().timeIntervalSince(modified) > cacheTTL { return nil }
        return try? Data(contentsOf: url)
    }

    private func staleFallback() -> [TickerCandidate] {
        loadCache(fresh: false).map { BinanceCatalog.map(exchangeInfoJSON: $0) } ?? []
    }

    private func writeCache(_ data: Data) {
        guard let url = cacheURL else { return }
        try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                                 withIntermediateDirectories: true)
        try? data.write(to: url, options: .atomic)
    }
}

private extension CharacterSet {
    // urlQueryAllowed keeps '&','=','+' literal which corrupt a query value; strip them so the q param is
    // safely escaped (e.g. "S&P" => "S%26P").
    static let urlQueryValueAllowed: CharacterSet = {
        var set = CharacterSet.urlQueryAllowed
        set.remove(charactersIn: "&=+?#")
        return set
    }()
}
