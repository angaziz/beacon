import XCTest
@testable import BeaconHubKit

final class TickerIDTests: XCTestCase {
    func testDeterministicAndBounded() {
        let cases: [(TickerSource, String)] = [
            (.binance, "BTCUSDT"), (.yahoo, "%5EGSPC"), (.yahoo, "EURUSD=X"), (.yahoo, "GC=F"),
        ]
        for (src, sym) in cases {
            let a = TickerID.make(src: src, sym: sym)
            let b = TickerID.make(src: src, sym: sym)
            XCTAssertEqual(a, b, "deterministic for \(src.rawValue):\(sym)")
            XCTAssertLessThanOrEqual(a.count, 15, "fits firmware FIN_ID_LEN-1 for \(sym): \(a)")
            XCTAssertFalse(a.isEmpty)
        }
    }

    func testDifferentInputsDiffer() {
        let ids = [
            TickerID.make(src: .binance, sym: "BTCUSDT"),
            TickerID.make(src: .yahoo,   sym: "BTCUSDT"),   // same sym, different source
            TickerID.make(src: .yahoo,   sym: "%5EGSPC"),
            TickerID.make(src: .yahoo,   sym: "%5EIXIC"),
            TickerID.make(src: .binance, sym: "ETHUSDT"),
        ]
        XCTAssertEqual(Set(ids).count, ids.count, "no collisions: \(ids)")
    }

    func testInvariantUnderListContext() {
        // The id depends only on (src, sym), never on neighbouring rows / list position.
        let standalone = TickerID.make(src: .yahoo, sym: "GC=F")
        let listed: [String] = ["AAA", "GC=F", "ZZZ"].map { TickerID.make(src: .yahoo, sym: $0) }
        XCTAssertEqual(listed[1], standalone)
    }
}

final class SymbolEncodingTests: XCTestCase {
    func testYahooPathRawSymbols() {
        // Raw Yahoo symbols => the exact path-segment bytes the firmware interpolates into
        // /v8/finance/chart/<sym>. '^' becomes %5E; '=' and digits stay literal (encode-once).
        let cases: [(raw: String, want: String)] = [
            ("^GSPC",    "%5EGSPC"),
            ("^IXIC",    "%5EIXIC"),
            ("^DJI",     "%5EDJI"),
            ("GC=F",     "GC=F"),
            ("CL=F",     "CL=F"),
            ("EURUSD=X", "EURUSD=X"),
        ]
        for c in cases {
            XCTAssertEqual(SymbolEncoding.yahooPath(c.raw), c.want, "raw \(c.raw)")
        }
    }
}

final class ConfigFrameTests: XCTestCase {
    private func row(_ src: TickerSource, _ sym: String, _ name: String,
                     _ kind: TickerKind, _ cadence: Int, _ stale: Int, _ basis: ChangeBasis) -> TickerRow {
        TickerRow(id: TickerID.make(src: src, sym: sym), src: src, sym: sym, name: name,
                  kind: kind, cadence: cadence, stale: stale, basis: basis)
    }

    func testSingleRowFrameShape() throws {
        let r = row(.yahoo, "%5EGSPC", "S&P 500", .index, 300, 600, .prevClose)
        let frames = try ConfigFrame.chunks(rows: [r], rev: 7)
        XCTAssertEqual(frames.count, 1)
        let data = frames[0]
        XCTAssertEqual(data.last, 0x0A, "newline-terminated")

        let obj = try JSONSerialization.jsonObject(with: data) as! [String: Any]
        XCTAssertEqual(obj["v"] as? Int, 1)
        XCTAssertEqual(Set(obj.keys), ["v", "config"])
        let cfg = obj["config"] as! [String: Any]
        XCTAssertEqual(cfg["rev"] as? Int, 7)
        XCTAssertEqual(cfg["part"] as? Int, 0)
        XCTAssertEqual(cfg["parts"] as? Int, 1)
        let tickers = cfg["tickers"] as! [[String: Any]]
        XCTAssertEqual(tickers.count, 1)
        let row = tickers[0]
        XCTAssertEqual(Set(row.keys), ["id", "src", "sym", "name", "kind", "cadence", "stale", "basis"])
        XCTAssertEqual(row["id"] as? String, r.id)
        XCTAssertEqual(row["src"] as? String, "yahoo")
        XCTAssertEqual(row["sym"] as? String, "%5EGSPC")
        XCTAssertEqual(row["name"] as? String, "S&P 500")
        XCTAssertEqual(row["kind"] as? String, "index")
        XCTAssertEqual(row["cadence"] as? Int, 300)
        XCTAssertEqual(row["stale"] as? Int, 600)
        XCTAssertEqual(row["basis"] as? String, "prev_close")   // ChangeBasis rawValue
    }

    func testManyRowsChunkCorrectly() throws {
        var rows = [TickerRow]()
        for i in 0..<16 {
            rows.append(row(.binance, "SYMBOL\(i)USDT", "Coin number \(i) with a long display name",
                            .crypto, 60, 600, .h24))
        }
        let frames = try ConfigFrame.chunks(rows: rows, rev: 9, maxBytes: 300)
        XCTAssertGreaterThan(frames.count, 1, "16 rows under a 300B budget must span multiple chunks")

        var reassembled = [String]()   // collect ids across chunks in part order
        for (i, data) in frames.enumerated() {
            XCTAssertLessThanOrEqual(data.count, 300, "chunk \(i) within budget")
            let cfg = (try JSONSerialization.jsonObject(with: data) as! [String: Any])["config"] as! [String: Any]
            XCTAssertEqual(cfg["rev"] as? Int, 9)
            XCTAssertEqual(cfg["part"] as? Int, i, "0-based part index")
            XCTAssertEqual(cfg["parts"] as? Int, frames.count, "parts consistent across chunks")
            for t in (cfg["tickers"] as! [[String: Any]]) { reassembled.append(t["id"] as! String) }
        }
        XCTAssertEqual(reassembled, rows.map { $0.id }, "rows reassembled in input order, none split")
    }

    func testManyRowsDefaultBudgetSingleChunk() throws {
        // Under the default 900B budget a modest 16-row list still chunks without splitting any row.
        let rows = (0..<16).map { row(.binance, "B\($0)USDT", "C\($0)", .crypto, 60, 600, .h24) }
        let frames = try ConfigFrame.chunks(rows: rows, rev: 1)
        let ids = try frames.flatMap { data -> [String] in
            let cfg = (try JSONSerialization.jsonObject(with: data) as! [String: Any])["config"] as! [String: Any]
            return (cfg["tickers"] as! [[String: Any]]).map { $0["id"] as! String }
        }
        XCTAssertEqual(ids, rows.map { $0.id })
        for data in frames { XCTAssertLessThanOrEqual(data.count, 900) }
    }

    func testEmptyRowsReturnsEmpty() throws {
        XCTAssertTrue(try ConfigFrame.chunks(rows: [], rev: 1).isEmpty)
    }

    func testRowTooLargeThrows() {
        let big = row(.yahoo, "%5EGSPC", String(repeating: "X", count: 500), .index, 300, 600, .prevClose)
        XCTAssertThrowsError(try ConfigFrame.chunks(rows: [big], rev: 1, maxBytes: 200)) { err in
            guard case ConfigFrameError.rowTooLarge = err else {
                return XCTFail("expected rowTooLarge, got \(err)")
            }
        }
    }
}

final class ConfigAckParseTests: XCTestCase {
    func testParseConfigAckOk() {
        let c = DeviceCommand.parse(Data(#"{"v":1,"cmd":"config_ack","rev":7,"ok":true,"count":8}"#.utf8))
        XCTAssertEqual(c, .configAck(rev: 7, ok: true, count: 8, err: nil))
    }

    func testParseConfigAckErr() {
        let c = DeviceCommand.parse(Data(#"{"v":1,"cmd":"config_ack","rev":7,"ok":false,"err":"too_many_tickers"}"#.utf8))
        XCTAssertEqual(c, .configAck(rev: 7, ok: false, count: nil, err: "too_many_tickers"))
    }

    func testPermissionStillParses() {
        XCTAssertEqual(
            DeviceCommand.parse(Data(#"{"v":1,"cmd":"permission","id":"req_abc","decision":"approve"}"#.utf8)),
            .permission(id: "req_abc", approve: true))
    }

    func testConfigAckRejectsMissingFields() {
        XCTAssertNil(DeviceCommand.parse(Data(#"{"v":1,"cmd":"config_ack","ok":true}"#.utf8)))     // no rev
        XCTAssertNil(DeviceCommand.parse(Data(#"{"v":1,"cmd":"config_ack","rev":7}"#.utf8)))       // no ok
    }
}
