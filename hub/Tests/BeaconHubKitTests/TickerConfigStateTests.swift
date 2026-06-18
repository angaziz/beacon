import XCTest
@testable import BeaconHubKit

final class TickerConfigStateTests: XCTestCase {

    private func row(_ sym: String) -> TickerRow {
        TickerRow(id: TickerID.make(src: .yahoo, sym: sym), src: .yahoo, sym: sym,
                  name: sym, kind: .index, cadence: 300, stale: 600, basis: .prevClose)
    }

    func testUpdatingBumpsRevByOneAndReplacesRows() {
        let s0 = TickerConfigState()
        XCTAssertEqual(s0.rev, 0)

        let s1 = s0.updating(rows: [row("a")])
        XCTAssertEqual(s1.rev, 1)
        XCTAssertEqual(s1.rows, [row("a")])

        let s2 = s1.updating(rows: [row("b"), row("c")])
        XCTAssertEqual(s2.rev, 2)
        XCTAssertEqual(s2.rows, [row("b"), row("c")])   // replaced, not appended
    }

    func testUpdatingSaturatesAtMax() {
        let s = TickerConfigState(rows: [], rev: .max)
        XCTAssertEqual(s.updating(rows: [row("a")]).rev, .max)   // no wrap to 0
    }

    func testCodableRoundTrip() throws {
        let original = TickerConfigState(rows: [row("a"), row("b")], rev: 7)
        let data = try JSONEncoder().encode(original)
        let decoded = try JSONDecoder().decode(TickerConfigState.self, from: data)
        XCTAssertEqual(decoded, original)
    }
}
