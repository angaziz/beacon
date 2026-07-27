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

    // --- installOmp / isOmpInstalled (file IO) ---

    private func ompPath() -> String { tmp.appendingPathComponent("beacon.ts").path }

    // Fresh install writes content that detection accepts as current.
    func testInstallOmpFreshWritesCurrent() throws {
        let path = ompPath()
        XCTAssertFalse(HooksInstaller.isOmpInstalled(extensionPath: path), "missing file => not installed")
        try HooksInstaller.installOmp(extensionPath: path)
        XCTAssertTrue(HooksInstaller.isOmpInstalled(extensionPath: path))
        XCTAssertEqual(try String(contentsOfFile: path, encoding: .utf8), OmpHooks.extensionSource)
    }

    // Detection is exact-content: truncated and version-bumped variants read as NOT current.
    func testIsOmpInstalledRejectsStale() throws {
        let path = ompPath()
        try String(OmpHooks.extensionSource.dropLast(80)).write(toFile: path, atomically: true, encoding: .utf8)
        XCTAssertFalse(HooksInstaller.isOmpInstalled(extensionPath: path), "truncated => not current")
        try OmpHooks.extensionSource.replacingOccurrences(of: "beacon-omp v4", with: "beacon-omp v40")
            .write(toFile: path, atomically: true, encoding: .utf8)
        XCTAssertFalse(HooksInstaller.isOmpInstalled(extensionPath: path), "v40 => not current")
    }

    // Install over an unrecognized file backs up the prior bytes and writes current content.
    func testInstallOmpBacksUpUnrecognized() throws {
        let fm = FileManager.default
        let path = ompPath()
        try "// my own extension\n".write(toFile: path, atomically: true, encoding: .utf8)
        try HooksInstaller.installOmp(extensionPath: path)
        XCTAssertTrue(HooksInstaller.isOmpInstalled(extensionPath: path))
        let backups = try fm.contentsOfDirectory(atPath: tmp.path).filter { $0.hasPrefix("beacon.ts.bak-") }
        XCTAssertEqual(backups.count, 1, "one timestamped backup of the prior file")
        let saved = try String(contentsOfFile: tmp.appendingPathComponent(backups[0]).path, encoding: .utf8)
        XCTAssertEqual(saved, "// my own extension\n", "backup preserves prior bytes")
    }

    // Install over an already-current file is a no-op: content unchanged, no backup made.
    func testInstallOmpIdempotentNoOp() throws {
        let fm = FileManager.default
        let path = ompPath()
        try HooksInstaller.installOmp(extensionPath: path)
        try HooksInstaller.installOmp(extensionPath: path)   // second install
        let backups = try fm.contentsOfDirectory(atPath: tmp.path).filter { $0.hasPrefix("beacon.ts.bak-") }
        XCTAssertTrue(backups.isEmpty, "current file left untouched, no backup")
        XCTAssertEqual(try String(contentsOfFile: path, encoding: .utf8), OmpHooks.extensionSource)
    }

    // Install through a symlinked beacon.ts keeps the symlink and updates its target.
    func testInstallOmpPreservesSymlink() throws {
        let fm = FileManager.default
        let real = tmp.appendingPathComponent("real-beacon.ts")
        let link = tmp.appendingPathComponent("beacon.ts")
        try "// old\n".write(to: real, atomically: true, encoding: .utf8)
        try fm.createSymbolicLink(at: link, withDestinationURL: real)
        try HooksInstaller.installOmp(extensionPath: link.path)
        XCTAssertEqual(try fm.destinationOfSymbolicLink(atPath: link.path), real.path,
                       "beacon.ts stays a symlink pointing at its target")
        XCTAssertEqual(try String(contentsOf: real, encoding: .utf8), OmpHooks.extensionSource)
    }
}
