import XCTest
@testable import BeaconHubKit

final class ReportAssemblerTests: XCTestCase {
    private func chunk(_ part: Int, _ parts: Int, _ rows: [TickerRow], rev: UInt32 = 0) -> DeviceCommand {
        .report(what: "tickers", rev: rev, part: part, parts: parts, rows: rows)
    }
    private func row(_ id: String) -> TickerRow {
        TickerRow(id: id, src: .binance, sym: "BTCUSDT", name: "BTC",
                  kind: .crypto, cadence: 60, stale: 600, basis: .h24)
    }

    func testSingleChunkAssembles() {
        var a = ReportAssembler()
        XCTAssertEqual(a.feed(chunk(0, 1, [row("a")])), .assembled([row("a")]))
    }

    func testMultiChunkAssemblesInOrder() {
        var a = ReportAssembler()
        XCTAssertEqual(a.feed(chunk(0, 2, [row("a")])), .pending)
        XCTAssertEqual(a.feed(chunk(1, 2, [row("b")])), .assembled([row("a"), row("b")]))
    }

    func testGapDrops() {
        var a = ReportAssembler()
        XCTAssertEqual(a.feed(chunk(0, 3, [row("a")])), .pending)
        XCTAssertEqual(a.feed(chunk(2, 3, [row("c")])), .dropped)   // skipped part 1
    }

    func testPartZeroRestarts() {
        var a = ReportAssembler()
        XCTAssertEqual(a.feed(chunk(0, 2, [row("a")])), .pending)
        XCTAssertEqual(a.feed(chunk(0, 2, [row("a")])), .pending)   // part 0 restarts (not a fault)
        XCTAssertEqual(a.feed(chunk(1, 2, [row("b")])), .assembled([row("a"), row("b")]))
    }

    func testPartsMismatchDrops() {
        var a = ReportAssembler()
        XCTAssertEqual(a.feed(chunk(0, 2, [row("a")])), .pending)
        XCTAssertEqual(a.feed(chunk(1, 3, [row("b")])), .dropped)   // parts changed mid-stream
    }

    func testRevMismatchDrops() {
        var a = ReportAssembler()
        XCTAssertEqual(a.feed(chunk(0, 2, [row("a")], rev: 0)), .pending)
        XCTAssertEqual(a.feed(chunk(1, 2, [row("b")], rev: 1)), .dropped)   // rev changed mid-stream
    }

    func testResetClearsPartial() {
        var a = ReportAssembler()
        XCTAssertEqual(a.feed(chunk(0, 2, [row("a")])), .pending)
        a.reset()
        XCTAssertEqual(a.feed(chunk(1, 2, [row("b")])), .dropped)   // no active accumulation after reset
    }

    func testIsPristine() {
        XCTAssertTrue(TickerConfigState().isPristine)                              // rev 0, empty
        XCTAssertFalse(TickerConfigState(rows: [row("a")], rev: 0).isPristine)     // has rows
        XCTAssertFalse(TickerConfigState(rows: [], rev: 1).isPristine)             // user emptied (rev>0)
    }
}
