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
        let cases: [(alias: String, expectedInput: Double)] = [
            ("sonnet", 3.0), ("opus", 5.0), ("haiku", 1.0),
        ]
        for c in cases {
            XCTAssertEqual(PricingTable.shared.pricing(for: c.alias)?.inputPerM, c.expectedInput, "alias=\(c.alias)")
        }
    }

    func testCacheTierMultipliers() {
        let p = PricingTable.shared.pricing(for: "claude-opus-4-8")!
        XCTAssertEqual(p.cacheReadPerM, 0.5)
        XCTAssertEqual(p.cacheWrite5mPerM, 6.25)
        XCTAssertEqual(p.cacheWrite1hPerM, 10.0)
    }

    func testUnknownModelIsNil() {
        XCTAssertNil(PricingTable.shared.pricing(for: "gpt-9-imaginary"))
    }
}
