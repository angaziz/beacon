import XCTest
import BeaconHubKit
@testable import beacon_hub

// Codex config install internals: the canonical [hooks.state] key derivation and the symlink-preserving
// atomic swap. installCodex() itself writes to fixed ~/.beacon + ~/.codex paths (not injectable), so we
// cover its two load-bearing seams directly.
final class HooksInstallerTests: XCTestCase {
    private var tmp: URL!

    override func setUpWithError() throws {
        tmp = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("beacon-hooks-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
    }
    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: tmp)
    }

    // --- canonicalConfigPath (pure) ---

    // With the file present, Codex canonicalizes the file itself, so its realpath (a resolved symlink
    // target) is the key path verbatim.
    func testCanonicalConfigPathUsesFileRealpathWhenPresent() {
        let key = HooksInstaller.canonicalConfigPath(
            fileRealpath: "/real/store/config.toml", dirRealpath: "/home/.codex", fileName: "config.toml")
        XCTAssertEqual(key, "/real/store/config.toml")
    }

    // With no file yet, the key is the canonical directory plus the filename (a fresh regular file).
    func testCanonicalConfigPathComposesDirWhenAbsent() {
        let key = HooksInstaller.canonicalConfigPath(
            fileRealpath: nil, dirRealpath: "/home/.codex", fileName: "config.toml")
        XCTAssertEqual(key, "/home/.codex/config.toml")
    }

    // --- realpathOrSelf ---

    func testRealpathOrSelfReturnsInputOnMissing() {
        let missing = "/no/such/beacon-\(UUID().uuidString)/config.toml"
        XCTAssertEqual(HooksInstaller.realpathOrSelf(missing), missing)
    }

    func testRealpathOrSelfResolvesSymlink() throws {
        let real = tmp.appendingPathComponent("real.toml")
        let link = tmp.appendingPathComponent("config.toml")
        try "x".write(to: real, atomically: true, encoding: .utf8)
        try FileManager.default.createSymbolicLink(at: link, withDestinationURL: real)
        XCTAssertEqual(HooksInstaller.realpathOrSelf(link.path), HooksInstaller.realpathOrSelf(real.path))
    }

    // --- atomicWriteThrough (file IO) ---

    // Writing through a resolved symlink target updates the real content and leaves the symlink a symlink
    // (never destroyed and replaced with a regular file).
    func testAtomicWriteThroughPreservesSymlink() throws {
        let fm = FileManager.default
        let real = tmp.appendingPathComponent("real.toml")
        let link = tmp.appendingPathComponent("config.toml")
        try "old".write(to: real, atomically: true, encoding: .utf8)
        try fm.createSymbolicLink(at: link, withDestinationURL: real)

        let target = HooksInstaller.realpathOrSelf(link.path)   // what installCodex passes
        try HooksInstaller.atomicWriteThrough(target: target, content: "new")

        var isDir: ObjCBool = false
        XCTAssertTrue(fm.fileExists(atPath: link.path, isDirectory: &isDir))
        XCTAssertEqual(try fm.destinationOfSymbolicLink(atPath: link.path), real.path,
                       "config.toml stays a symlink pointing at its target")
        XCTAssertEqual(try String(contentsOf: real, encoding: .utf8), "new")
        XCTAssertEqual(try String(contentsOf: link, encoding: .utf8), "new", "reads through the link")
    }

    // Overwriting a regular file replaces content atomically and leaves no stray temp files behind.
    func testAtomicWriteThroughReplacesRegularFile() throws {
        let fm = FileManager.default
        let file = tmp.appendingPathComponent("config.toml")
        try "old".write(to: file, atomically: true, encoding: .utf8)
        try fm.setAttributes([.posixPermissions: 0o600], ofItemAtPath: file.path)

        try HooksInstaller.atomicWriteThrough(target: file.path, content: "new")

        XCTAssertEqual(try String(contentsOf: file, encoding: .utf8), "new")
        let mode = (try fm.attributesOfItem(atPath: file.path)[.posixPermissions]) as? NSNumber
        XCTAssertEqual(mode?.int16Value, 0o600, "preserves the existing file mode")
        let leftover = try fm.contentsOfDirectory(atPath: tmp.path).filter { $0.contains("config.toml.beacon.") }
        XCTAssertTrue(leftover.isEmpty, "temp file renamed away, none left behind")
    }

    // --- installPi / isPiInstalled (file IO) ---

    private func piPath() -> String { tmp.appendingPathComponent("beacon.ts").path }

    func testInstallPiFreshWritesCurrent() throws {
        let path = piPath()
        XCTAssertFalse(HooksInstaller.isPiInstalled(extensionPath: path), "missing file => not installed")
        try HooksInstaller.installPi(extensionPath: path)
        XCTAssertTrue(HooksInstaller.isPiInstalled(extensionPath: path))
        XCTAssertEqual(try String(contentsOfFile: path, encoding: .utf8), PiHooks.extensionSource)
    }

    func testInstallPiBacksUpUnrecognized() throws {
        let fileManager = FileManager.default
        let path = piPath()
        try "// my own extension\n".write(toFile: path, atomically: true, encoding: .utf8)
        try HooksInstaller.installPi(extensionPath: path)

        let backups = try fileManager.contentsOfDirectory(atPath: tmp.path).filter { $0.hasPrefix("beacon.ts.bak-") }
        XCTAssertEqual(backups.count, 1, "one timestamped backup of the prior file")
        XCTAssertTrue(HooksInstaller.isPiInstalled(extensionPath: path))
    }

    func testPpsInstallPreservesOrderAndBacksUpOriginal() throws {
        let config = tmp.appendingPathComponent("config.json")
        let original = "{\n  \"permission\": {\"bash\": {\"*\": \"allow\", \"rm *\": \"ask\"}},\n  \"authorizerChain\": [\"first\"]\n}\n"
        try original.write(to: config, atomically: true, encoding: .utf8)
        try HooksInstaller.installPpsAuthorizerChain(configPath: config.path)

        let installed = try String(contentsOf: config, encoding: .utf8)
        XCTAssertEqual(installed, original.replacingOccurrences(of: "]", with: ", \"beacon\"]"))
        let backups = try FileManager.default.contentsOfDirectory(atPath: tmp.path).filter { $0.hasPrefix("config.json.bak-") }
        XCTAssertEqual(backups.count, 1)
        XCTAssertEqual(try String(contentsOf: tmp.appendingPathComponent(backups[0]), encoding: .utf8), original)
        try HooksInstaller.installPpsAuthorizerChain(configPath: config.path)
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: tmp.path).filter { $0.hasPrefix("config.json.bak-") }.count, 1)
    }

    func testPpsInstallDoesNotCreateOrChangeInvalidConfig() throws {
        let absent = tmp.appendingPathComponent("absent.json")
        try HooksInstaller.installPpsAuthorizerChain(configPath: absent.path)
        XCTAssertFalse(FileManager.default.fileExists(atPath: absent.path))

        let invalid = tmp.appendingPathComponent("invalid.json")
        let original = "{\"authorizerChain\":\"beacon\"}"
        try original.write(to: invalid, atomically: true, encoding: .utf8)
        try HooksInstaller.installPpsAuthorizerChain(configPath: invalid.path)
        XCTAssertEqual(try String(contentsOf: invalid, encoding: .utf8), original)
        XCTAssertFalse(HooksInstaller.isPiInstalled(extensionPath: piPath(), ppsConfigPath: invalid.path))
    }

    func testPiReadyRequiresValidPpsChain() throws {
        let path = piPath()
        let config = tmp.appendingPathComponent("config.json")
        try HooksInstaller.installPi(extensionPath: path)
        let cases: [(name: String, config: String, ready: Bool)] = [
            ("chain with beacon", "{\"authorizerChain\":[\"beacon\"]}", true),
            ("pps 39 keys with beacon", "{\"promptMaxRows\":8,\"authorizerChain\":[\"beacon\"]}", true),
            ("chain without beacon", "{\"authorizerChain\":[\"first\"]}", false),
            ("invalid chain", "{\"authorizerChain\":[\"beacon\", 1]}", false),
        ]
        for entry in cases {
            try entry.config.write(to: config, atomically: true, encoding: .utf8)
            XCTAssertEqual(HooksInstaller.isPiInstalled(extensionPath: path, ppsConfigPath: config.path), entry.ready, entry.name)
        }
    }

}
