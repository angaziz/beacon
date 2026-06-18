import XCTest
@testable import BeaconHubKit

final class BinanceCatalogTests: XCTestCase {
    // TRADING BTCUSDT + ETHUSDC kept; BREAK (non-TRADING) and BTCEUR (other quote) filtered.
    private let json = Data(#"""
    {"symbols":[
      {"symbol":"BTCUSDT","status":"TRADING","baseAsset":"BTC","quoteAsset":"USDT"},
      {"symbol":"ETHUSDC","status":"TRADING","baseAsset":"ETH","quoteAsset":"USDC"},
      {"symbol":"XRPUSDT","status":"BREAK","baseAsset":"XRP","quoteAsset":"USDT"},
      {"symbol":"BTCEUR","status":"TRADING","baseAsset":"BTC","quoteAsset":"EUR"}
    ]}
    """#.utf8)

    func testMapFiltersAndMaps() {
        let got = BinanceCatalog.map(exchangeInfoJSON: json)
        XCTAssertEqual(got.map { $0.row.sym }, ["BTCUSDT", "ETHUSDC"], "BREAK + EUR-quote filtered")

        let btc = got[0]
        XCTAssertEqual(btc.sourceLabel, "Binance")
        XCTAssertEqual(btc.row.src, .binance)
        XCTAssertEqual(btc.row.kind, .crypto)
        XCTAssertEqual(btc.row.sym, "BTCUSDT", "raw pair, no encoding")
        XCTAssertEqual(btc.row.name, "BTC/USDT")
        XCTAssertEqual(btc.row.cadence, 60)
        XCTAssertEqual(btc.row.stale, 600)
        XCTAssertEqual(btc.row.basis, .h24)
        XCTAssertFalse(btc.row.id.isEmpty)
        XCTAssertLessThanOrEqual(btc.row.id.count, 15)
        XCTAssertEqual(btc.row.id, TickerID.make(src: .binance, sym: "BTCUSDT"))
    }

    func testSearchMatchesAndOrdersUSDTFirst() {
        let candidates = BinanceCatalog.map(exchangeInfoJSON: Data(#"""
        {"symbols":[
          {"symbol":"BTCUSDC","status":"TRADING","baseAsset":"BTC","quoteAsset":"USDC"},
          {"symbol":"BTCUSDT","status":"TRADING","baseAsset":"BTC","quoteAsset":"USDT"},
          {"symbol":"ETHUSDT","status":"TRADING","baseAsset":"ETH","quoteAsset":"USDT"}
        ]}
        """#.utf8))

        let hits = BinanceCatalog.search("btc", in: candidates)
        XCTAssertEqual(hits.map { $0.row.sym }, ["BTCUSDT", "BTCUSDC"], "case-insensitive; USDT first")
    }

    func testMalformedReturnsEmpty() {
        XCTAssertTrue(BinanceCatalog.map(exchangeInfoJSON: Data("not json".utf8)).isEmpty)
        XCTAssertTrue(BinanceCatalog.map(exchangeInfoJSON: Data("{}".utf8)).isEmpty)
    }
}

final class YahooCatalogTests: XCTestCase {
    private let json = Data(#"""
    {"quotes":[
      {"symbol":"^GSPC","shortname":"S&P 500","longname":"S&P 500 Index","quoteType":"INDEX"},
      {"symbol":"SPY","shortname":"SPDR S&P 500 ETF","quoteType":"ETF"},
      {"symbol":"BTC-USD","shortname":"Bitcoin USD","quoteType":"CRYPTOCURRENCY"},
      {"symbol":"EURUSD=X","shortname":"EUR/USD","quoteType":"CURRENCY"},
      {"symbol":"AAPL","shortname":"Apple Inc.","quoteType":"EQUITY"},
      {"symbol":"VFIAX","shortname":"Vanguard 500","quoteType":"MUTUALFUND"}
    ]}
    """#.utf8)

    func testMapKindMappingAndEncoding() {
        let got = YahooCatalog.map(searchJSON: json)
        // MUTUALFUND skipped; EQUITY treated as etf.
        let bySym = Dictionary(uniqueKeysWithValues: got.map { ($0.row.sym, $0) })

        let cases: [(sym: String, kind: TickerKind, name: String)] = [
            ("%5EGSPC", .index, "S&P 500"),   // ^ percent-encoded once
            ("SPY", .etf, "SPDR S&P 500 ETF"),
            ("BTC-USD", .crypto, "Bitcoin USD"),
            ("EURUSD=X", .fx, "EUR/USD"),
            ("AAPL", .etf, "Apple Inc."),     // EQUITY => etf
        ]
        XCTAssertEqual(got.count, cases.count, "MUTUALFUND dropped")
        for c in cases {
            guard let cand = bySym[c.sym] else { return XCTFail("missing \(c.sym)") }
            XCTAssertEqual(cand.row.kind, c.kind, "kind for \(c.sym)")
            XCTAssertEqual(cand.row.name, c.name, "name from shortname for \(c.sym)")
            XCTAssertEqual(cand.sourceLabel, "Yahoo")
            XCTAssertEqual(cand.row.src, .yahoo)
            XCTAssertEqual(cand.row.cadence, 300)
            XCTAssertEqual(cand.row.stale, 600)
            XCTAssertEqual(cand.row.basis, .prevClose)
            XCTAssertEqual(cand.row.id, TickerID.make(src: .yahoo, sym: c.sym))
            XCTAssertLessThanOrEqual(cand.row.id.count, 15)
        }
    }

    func testNameFallbackToLongnameThenSymbol() {
        let got = YahooCatalog.map(searchJSON: Data(#"""
        {"quotes":[
          {"symbol":"^DJI","longname":"Dow Jones","quoteType":"INDEX"},
          {"symbol":"^IXIC","quoteType":"INDEX"}
        ]}
        """#.utf8))
        XCTAssertEqual(got.first { $0.row.sym == "%5EDJI" }?.row.name, "Dow Jones")
        XCTAssertEqual(got.first { $0.row.sym == "%5EIXIC" }?.row.name, "^IXIC")
    }

    func testMalformedReturnsEmpty() {
        XCTAssertTrue(YahooCatalog.map(searchJSON: Data("not json".utf8)).isEmpty)
        XCTAssertTrue(YahooCatalog.map(searchJSON: Data("{}".utf8)).isEmpty)
    }
}

final class TickerMergeTests: XCTestCase {
    func testBothSourcesKeptAndOrdered() {
        let binance = BinanceCatalog.map(exchangeInfoJSON: Data(#"""
        {"symbols":[
          {"symbol":"BTCUSDT","status":"TRADING","baseAsset":"BTC","quoteAsset":"USDT"},
          {"symbol":"BTCUSDC","status":"TRADING","baseAsset":"BTC","quoteAsset":"USDC"}
        ]}
        """#.utf8))
        let yahoo = YahooCatalog.map(searchJSON: Data(#"""
        {"quotes":[
          {"symbol":"BTC-USD","shortname":"Bitcoin USD","quoteType":"CRYPTOCURRENCY"},
          {"symbol":"^GSPC","shortname":"S&P 500","quoteType":"INDEX"}
        ]}
        """#.utf8))

        let merged = TickerMerge.unify(binance: binance, yahoo: yahoo)

        // BTC present on both Binance (BTCUSDT) and Yahoo (BTC-USD) => two labeled candidates, not deduped.
        let btcLabels = merged.filter { $0.row.name.uppercased().contains("BTC") || $0.row.sym.contains("BTC") }
            .map { $0.sourceLabel }
        XCTAssertTrue(btcLabels.contains("Binance") && btcLabels.contains("Yahoo"))

        // Deterministic: crypto first (USDT < USDC < non-Binance crypto), index last.
        XCTAssertEqual(merged.map { $0.row.sym }, ["BTCUSDT", "BTCUSDC", "BTC-USD", "%5EGSPC"])
    }
}
