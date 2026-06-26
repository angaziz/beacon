import XCTest
@testable import BeaconHubKit

final class SessionsFrameTests: XCTestCase {
    func testEncodesWireShape() throws {
        let f = SessionsFrame([
            Session(id: "s3", label: "beacon · fix/109", state: .attention, ts: 1719400000),
            Session(id: "s1", label: "api · main", state: .working, ts: 1719399860),
        ])
        let s = String(decoding: try f.encoded(), as: UTF8.self)
        XCTAssertTrue(s.hasSuffix("\n"))
        XCTAssertTrue(s.contains("\"v\":1"))
        XCTAssertTrue(s.contains("\"state\":\"attention\""))
        XCTAssertTrue(s.contains("\"state\":\"working\""))
    }

    // Worst case MUST stay < HUB_FRAME_MAX (1024). 5 rows, max-length id + label.
    func testWorstCaseUnderFrameMax() throws {
        let rows = (0..<SessionLimits.maxCount).map { _ in
            Session(id: "s99999", label: String(repeating: "W", count: SessionLimits.labelMaxChars),
                    state: .waitingQueued, ts: 1719400000)
        }
        let bytes = try SessionsFrame(rows).encoded().count
        XCTAssertLessThan(bytes, 1024, "sessions frame worst case must fit HUB_FRAME_MAX; got \(bytes)")
    }
}
