import Foundation

// Source adapters (issue #92, design 2026-06-17 §4): turn each source's universe into canonical
// TickerRows, then merge into one labeled, deterministically-ordered candidate list for the UI.
// PURE MAPPING ONLY -- the network fetch lives in beacon-hub/TickerSearch.swift, which feeds the raw
// JSON Data here. Defaults (cadence/stale/basis) are keyed by (source, kind) and frozen as constants
// below. Malformed JSON never crashes: every map returns [] and skips rows it cannot classify.

// Trim a string to <= maxBytes of UTF-8 on a character boundary (the device counts bytes, not glyphs).
// Used so a long Yahoo company name fits the device's name buffer instead of being rejected as malformed.
func clampUTF8(_ s: String, _ maxBytes: Int) -> String {
    if s.utf8.count <= maxBytes { return s }
    var out = "", n = 0
    for ch in s {
        let b = String(ch).utf8.count
        if n + b > maxBytes { break }
        out.append(ch); n += b
    }
    return out
}

// One search/merge result: the canonical row plus a human source label for UI chips ("Binance"/"Yahoo").
public struct TickerCandidate: Equatable {
    public var row: TickerRow
    public var sourceLabel: String

    public init(row: TickerRow, sourceLabel: String) {
        self.row = row
        self.sourceLabel = sourceLabel
    }
}

// Per-(source,kind) defaults. Binance is always crypto (60s/600s/24h); Yahoo varies by kind but shares
// the same cadence/stale/basis across kinds (300s/600s/prev_close) per the design's single Yahoo profile.
enum TickerDefaults {
    static let binanceCrypto = (cadence: 60, stale: 600, basis: ChangeBasis.h24)
    static let yahoo = (cadence: 300, stale: 600, basis: ChangeBasis.prevClose)
}

public enum BinanceCatalog {
    private static let allowedQuotes: Set<String> = ["USDT", "USDC", "FDUSD"]
    public static let label = "Binance"

    // exchangeInfo => candidates. Keeps status==TRADING with quoteAsset in {USDT,USDC,FDUSD}; sym is the
    // raw pair ("BTCUSDT"), name is the base asset (or "BASE/QUOTE" when both are present). Skips anything
    // missing required fields. Order preserved as the API returns it (search re-orders).
    public static func map(exchangeInfoJSON: Data) -> [TickerCandidate] {
        guard let obj = try? JSONSerialization.jsonObject(with: exchangeInfoJSON) as? [String: Any],
              let symbols = obj["symbols"] as? [[String: Any]]
        else { return [] }

        var out = [TickerCandidate]()
        for s in symbols {
            guard let status = s["status"] as? String, status == "TRADING",
                  let symbol = s["symbol"] as? String, !symbol.isEmpty,
                  let quote = s["quoteAsset"] as? String, allowedQuotes.contains(quote)
            else { continue }
            guard symbol.utf8.count <= TickerLimits.symMaxBytes else { continue }   // device can't store it
            let base = (s["baseAsset"] as? String) ?? symbol
            let name = clampUTF8(base.isEmpty ? symbol : "\(base)/\(quote)", TickerLimits.nameMaxBytes)
            let d = TickerDefaults.binanceCrypto
            let row = TickerRow(id: TickerID.make(src: .binance, sym: symbol), src: .binance, sym: symbol,
                                name: name, kind: .crypto, cadence: d.cadence, stale: d.stale, basis: d.basis)
            out.append(TickerCandidate(row: row, sourceLabel: label))
        }
        return out
    }

    // Local filter over the cached candidate set: case-insensitive substring match on the base asset or
    // the raw symbol. USDT-quoted matches sort before USDC/FDUSD for the same base (the canonical pair).
    public static func search(_ query: String, in candidates: [TickerCandidate]) -> [TickerCandidate] {
        let q = query.uppercased()
        guard !q.isEmpty else { return [] }
        let matches = candidates.filter { c in
            let sym = c.row.sym.uppercased()
            let base = baseAsset(of: c.row.sym).uppercased()
            return sym.contains(q) || base.contains(q)
        }
        return matches.sorted { a, b in
            let qa = quoteRank(a.row.sym), qb = quoteRank(b.row.sym)
            if qa != qb { return qa < qb }
            return a.row.sym < b.row.sym
        }
    }

    // Quote precedence for ordering: USDT(0) < USDC(1) < FDUSD(2) < other(3).
    private static func quoteRank(_ symbol: String) -> Int {
        if symbol.hasSuffix("USDT") { return 0 }
        if symbol.hasSuffix("USDC") { return 1 }
        if symbol.hasSuffix("FDUSD") { return 2 }
        return 3
    }

    // Strip a known quote suffix to recover the base for matching ("BTCUSDT" => "BTC").
    private static func baseAsset(of symbol: String) -> String {
        for quote in ["FDUSD", "USDT", "USDC"] where symbol.hasSuffix(quote) {
            return String(symbol.dropLast(quote.count))
        }
        return symbol
    }
}

public enum YahooCatalog {
    public static let label = "Yahoo"

    // Yahoo search endpoint quotes => candidates. quoteType maps to kind; unclassifiable types are skipped:
    //   INDEX => index, ETF => etf, CRYPTOCURRENCY => crypto, CURRENCY => fx,
    //   EQUITY => etf (no equity kind on the device; equities ride the etf visual/cadence profile),
    //   MUTUALFUND / anything else => skip.
    // sym is the raw symbol percent-encoded EXACTLY ONCE via SymbolEncoding.yahooPath; name prefers
    // shortname, then longname, then the symbol itself.
    public static func map(searchJSON: Data) -> [TickerCandidate] {
        guard let obj = try? JSONSerialization.jsonObject(with: searchJSON) as? [String: Any],
              let quotes = obj["quotes"] as? [[String: Any]]
        else { return [] }

        var out = [TickerCandidate]()
        for q in quotes {
            guard let symbol = q["symbol"] as? String, !symbol.isEmpty,
                  let quoteType = q["quoteType"] as? String,
                  let kind = kind(for: quoteType)
            else { continue }
            let rawName = (q["shortname"] as? String).flatMap { $0.isEmpty ? nil : $0 }
                ?? (q["longname"] as? String).flatMap { $0.isEmpty ? nil : $0 }
                ?? symbol
            let d = TickerDefaults.yahoo
            let sym = SymbolEncoding.yahooPath(symbol)
            guard sym.utf8.count <= TickerLimits.symMaxBytes else { continue }       // device can't store it
            let name = clampUTF8(rawName, TickerLimits.nameMaxBytes)                 // long company names get trimmed
            let row = TickerRow(id: TickerID.make(src: .yahoo, sym: sym), src: .yahoo, sym: sym,
                                name: name, kind: kind, cadence: d.cadence, stale: d.stale, basis: d.basis)
            out.append(TickerCandidate(row: row, sourceLabel: label))
        }
        return out
    }

    private static func kind(for quoteType: String) -> TickerKind? {
        switch quoteType.uppercased() {
        case "INDEX": return .index
        case "ETF": return .etf
        case "CRYPTOCURRENCY": return .crypto
        case "CURRENCY": return .fx
        case "EQUITY": return .etf   // no equity kind; treat as etf for display
        default: return nil          // MUTUALFUND and unknown types are unclassifiable
        }
    }
}

public enum TickerMerge {
    // Combine both source result sets into one list for the UI. Duplicates across sources are KEPT (a
    // symbol on both Binance and Yahoo shows twice, each with its own sourceLabel) -- no cross-source
    // dedup. Deterministic order: crypto first (USDT before USDC/FDUSD within a base), then the rest, each
    // group sorted by display name then sym. Stable so equal-keyed candidates keep their input order.
    public static func unify(binance: [TickerCandidate], yahoo: [TickerCandidate]) -> [TickerCandidate] {
        let all = binance + yahoo
        return all.enumerated()
            .sorted { lhs, rhs in
                let a = lhs.element, b = rhs.element
                let ka = sortKey(a), kb = sortKey(b)
                if ka != kb { return ka < kb }
                if a.row.name != b.row.name {
                    return a.row.name.localizedCaseInsensitiveCompare(b.row.name) == .orderedAscending
                }
                if a.row.sym != b.row.sym { return a.row.sym < b.row.sym }
                return lhs.offset < rhs.offset   // stable tiebreak on input order
            }
            .map { $0.element }
    }

    // Primary sort bucket: crypto kinds first (0), then non-crypto (1). Within crypto, the Binance quote
    // suffix orders USDT < USDC < FDUSD; non-Binance crypto and non-crypto carry no quote sub-rank.
    private static func sortKey(_ c: TickerCandidate) -> Int {
        guard c.row.kind == .crypto else { return 100 }
        if c.row.src == .binance {
            if c.row.sym.hasSuffix("USDT") { return 0 }
            if c.row.sym.hasSuffix("USDC") { return 1 }
            if c.row.sym.hasSuffix("FDUSD") { return 2 }
            return 3
        }
        return 4   // non-Binance crypto (e.g. Yahoo BTC-USD) after Binance pairs, before non-crypto
    }
}
