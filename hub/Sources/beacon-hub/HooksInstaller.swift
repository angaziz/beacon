import Foundation
import BeaconHubKit

// Detect + install the beacon Claude Code hooks. Detection reads ~/.claude/settings.json and defers
// the dict -> Bool decision to BeaconHubKit.HooksDetection (unit-tested). Install shells out to the
// bundled `build-app.sh install-hooks` -- the single source of truth for the careful, idempotent,
// backup-making jq merge -- rather than reimplementing it in Swift.
enum HooksInstaller {

    // Stable, space-free install path. The .app bundle's Contents/Resources/ contains a space, which
    // build-app.sh only warns about and writes into an unquoted statusLine command that won't parse at
    // the CC layer. We copy the shim here and pass it via BEACON_SHIM so install + detection agree.
    static let shimInstallPath = NSString(string: "~/.beacon/beacon-statusline").expandingTildeInPath

    private static var defaultSettingsURL: URL {
        URL(fileURLWithPath: NSString(string: "~/.claude/settings.json").expandingTildeInPath)
    }

    struct HooksError: LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }

    // Path-injectable core (testable shape; the real read uses the defaults). Any read/parse failure
    // is treated as "not installed".
    static func isInstalled(settingsURL: URL? = nil, shimPath: String = shimInstallPath) -> Bool {
        let url = settingsURL ?? defaultSettingsURL
        guard let data = try? Data(contentsOf: url),
              let settings = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
        else { return false }
        return HooksDetection.isInstalled(settings: settings, shimPath: shimPath)
    }

    static func install() throws {
        let fm = FileManager.default
        let shimDir = (shimInstallPath as NSString).deletingLastPathComponent
        try fm.createDirectory(atPath: shimDir, withIntermediateDirectories: true)

        let bundledShim = try resolve(resource: "beacon-statusline", devPath: "statusline-shim/beacon-statusline")
        if fm.fileExists(atPath: shimInstallPath) { try fm.removeItem(atPath: shimInstallPath) }
        try fm.copyItem(atPath: bundledShim, toPath: shimInstallPath)
        try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: shimInstallPath)

        let script = try resolve(resource: "build-app.sh", devPath: "build-app.sh")
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/bash")
        process.arguments = [script, "install-hooks"]
        var env = ProcessInfo.processInfo.environment
        env["BEACON_SHIM"] = shimInstallPath
        process.environment = env
        let stderr = Pipe()
        process.standardError = stderr
        process.standardOutput = Pipe()   // drain stdout so a large report can't fill the pipe + block

        try process.run()
        process.waitUntilExit()
        guard process.terminationStatus == 0 else {
            let data = stderr.fileHandleForReading.readDataToEndOfFile()
            let msg = String(data: data, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines)
            throw HooksError(message: msg?.isEmpty == false ? msg! : "Install failed (exit \(process.terminationStatus)).")
        }
    }

    // Bundled (shipped .app) path takes precedence; dev fallback is the repo's hub/ dir next to Package.swift.
    private static func resolve(resource: String, devPath: String) throws -> String {
        if let bundled = Bundle.main.path(forResource: resource, ofType: nil) { return bundled }
        let dev = URL(fileURLWithPath: #filePath)               // .../hub/Sources/beacon-hub/HooksInstaller.swift
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent(devPath).path
        guard FileManager.default.fileExists(atPath: dev) else {
            throw HooksError(message: "Missing bundled resource: \(resource)")
        }
        return dev
    }
}
