import XCTest
import BeaconHubKit
@testable import beacon_hub

// Codex is usage-only (issue #160): it keeps its usage source but offers no Coding Buddy toggle and
// needs no hook setup.
final class CodexProviderTests: XCTestCase {
    func testCodexIsUsageOnly() {
        let p = CodexProvider(usageSession: URLSession(configuration: .ephemeral))
        XCTAssertEqual(p.descriptor.id, "codex")
        XCTAssertEqual(p.descriptor.capabilities, [.usage])
        XCTAssertFalse(p.descriptor.supportsBuddy)
        XCTAssertNotNil(p.usageSource)
        XCTAssertTrue(HooksInstaller.isInstalled(providerID: "codex"), "no hook setup to offer")
    }
}
