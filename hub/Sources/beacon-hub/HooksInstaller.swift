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
    static let sessionShimInstallPath = NSString(string: "~/.beacon/beacon-session").expandingTildeInPath

    // Codex buddy shim install path (same ~/.beacon/ home as the Claude statusline shim). The literal
    // command string written into ~/.codex/config.toml AND fed into the trust hash.
    static let codexShimInstallPath = NSString(string: "~/.beacon/beacon-codex-hook").expandingTildeInPath

    // omp managed extension install path. omp auto-discovers every module in ~/.omp/agent/extensions/,
    // so install = write one self-contained file (issue #136). Wholly Beacon-managed; see OmpHooks.
    static let ompExtensionInstallPath =
        NSString(string: "~/.omp/agent/extensions/beacon.ts").expandingTildeInPath

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

    // Per-provider dispatch: install only the named provider's hooks. Unknown ids no-op (a provider
    // without a hook install path is "ready" by default).
    static func install(providerID: String) throws {
        switch providerID {
        case "claude": try installClaude()
        case "codex":  try installCodex()
        case "omp":    try installOmp()
        default:       break
        }
    }

    // Per-provider detection dispatch; providers with no hook path report installed (nothing to do).
    static func isInstalled(providerID: String) -> Bool {
        switch providerID {
        case "claude": return isInstalled()
        case "codex":  return isCodexInstalled()
        case "omp":    return isOmpInstalled()
        default:       return true
        }
    }

    // Install the managed omp extension to ~/.omp/agent/extensions/beacon.ts. Idempotent: a byte-current
    // file is left untouched. An existing UNrecognized file (user's own or a stale Beacon version) is
    // backed up to <target>.bak-<stamp> before overwrite -- the path is user-owned until proven managed,
    // matching the Claude/Codex installers' backup safety. Writes through a symlink (resolves the target).
    static func installOmp(extensionPath: String = ompExtensionInstallPath) throws {
        let fm = FileManager.default
        let dir = (extensionPath as NSString).deletingLastPathComponent
        try fm.createDirectory(atPath: dir, withIntermediateDirectories: true)

        let exists = fm.fileExists(atPath: extensionPath)
        let writeTarget = exists ? realpathOrSelf(extensionPath) : extensionPath
        let current = (try? String(contentsOfFile: writeTarget, encoding: .utf8)) ?? ""
        if exists, OmpHooks.isCurrent(current) { return }   // already current: no-op

        if exists, !current.isEmpty {
            let stamp = Self.backupStamp.string(from: Date())
            let backup = "\(writeTarget).bak-\(stamp)"
            do {
                try fm.copyItem(atPath: writeTarget, toPath: backup)
            } catch {
                FileHandle.standardError.write(Data(
                    "[beacon-hub] omp: WARNING could not back up \(writeTarget) before overwrite: \(error.localizedDescription)\n".utf8))
            }
        }
        try Self.atomicWriteThrough(target: writeTarget, content: OmpHooks.extensionSource)
    }

    // True iff ~/.omp/agent/extensions/beacon.ts is byte-current with OmpHooks.extensionSource. Any read
    // failure (missing/unreadable) => not installed. Exact-content check (not a marker substring), so a
    // truncated/edited/older-or-newer file offers reinstall in Settings.
    static func isOmpInstalled(extensionPath: String = ompExtensionInstallPath) -> Bool {
        guard let content = try? String(contentsOfFile: extensionPath, encoding: .utf8) else { return false }
        return OmpHooks.isCurrent(content)
    }

    static func installClaude() throws {
        let fm = FileManager.default
        let shimDir = (shimInstallPath as NSString).deletingLastPathComponent
        try fm.createDirectory(atPath: shimDir, withIntermediateDirectories: true)

        let bundledShim = try resolve(resource: "beacon-statusline", devPath: "statusline-shim/beacon-statusline")
        if fm.fileExists(atPath: shimInstallPath) { try fm.removeItem(atPath: shimInstallPath) }
        try fm.copyItem(atPath: bundledShim, toPath: shimInstallPath)
        try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: shimInstallPath)

        let bundledSessionShim = try resolve(resource: "beacon-session", devPath: "statusline-shim/beacon-session")
        if fm.fileExists(atPath: sessionShimInstallPath) { try fm.removeItem(atPath: sessionShimInstallPath) }
        try fm.copyItem(atPath: bundledSessionShim, toPath: sessionShimInstallPath)
        try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: sessionShimInstallPath)

        let script = try resolve(resource: "build-app.sh", devPath: "build-app.sh")
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/bash")
        process.arguments = [script, "install-hooks"]
        var env = ProcessInfo.processInfo.environment
        env["BEACON_SHIM"] = shimInstallPath
        env["BEACON_SESSION"] = sessionShimInstallPath
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

    // ~/.codex/config.toml home (honors CODEX_HOME, matching Codex's own resolution).
    private static var codexConfigURL: URL {
        let home = ProcessInfo.processInfo.environment["CODEX_HOME"]
            ?? NSString(string: "~/.codex").expandingTildeInPath
        return URL(fileURLWithPath: home).appendingPathComponent("config.toml")
    }

    // True iff ~/.codex/config.toml carries Beacon's managed hook block wiring the installed shim.
    // Independent of Claude detection (keeps that path untouched); powers the menubar Codex hook state.
    static func isCodexInstalled(configURL: URL? = nil, shimCommand: String = codexShimInstallPath) -> Bool {
        let url = configURL ?? codexConfigURL
        guard let data = try? Data(contentsOf: url),
              let text = String(data: data, encoding: .utf8) else { return false }
        return CodexHooks.isInstalled(configText: text, shimCommand: shimCommand)
    }

    // Install the Codex buddy shim + a managed [hooks] block into ~/.codex/config.toml. The block is
    // idempotent (re-install rewrites only the marker-delimited section) and self-trusting: it writes
    // the [hooks.state] trusted_hash Codex requires so the freshly installed command hook RUNS with no
    // interactive trust step (verified against codex-cli 0.140.0; see hub/CONTRACT.md C.5).
    static func installCodex() throws {
        let fm = FileManager.default
        let shimDir = (codexShimInstallPath as NSString).deletingLastPathComponent
        try fm.createDirectory(atPath: shimDir, withIntermediateDirectories: true)

        let bundledShim = try resolve(resource: "beacon-codex-hook", devPath: "statusline-shim/beacon-codex-hook")
        if fm.fileExists(atPath: codexShimInstallPath) { try fm.removeItem(atPath: codexShimInstallPath) }
        try fm.copyItem(atPath: bundledShim, toPath: codexShimInstallPath)
        try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: codexShimInstallPath)

        let configURL = codexConfigURL
        let codexDir = configURL.deletingLastPathComponent()
        try fm.createDirectory(at: codexDir, withIntermediateDirectories: true)

        // Read existing config (preserved verbatim outside the managed block).
        let existing = (try? String(contentsOf: configURL, encoding: .utf8)) ?? ""

        // If config.toml is itself a symlink (common with dotfile managers), resolve it to the real
        // target so we write THROUGH the link (never destroy it) and key the trust state by the path
        // Codex actually sees. A fresh (non-existent) config becomes a regular file at configURL.
        let fileExists = fm.fileExists(atPath: configURL.path)
        let writeTarget = fileExists ? realpathOrSelf(configURL.path) : configURL.path
        let writeTargetURL = URL(fileURLWithPath: writeTarget)

        // Timestamped backup of the resolved target (its real content), matching the Claude installer's
        // safety. When there was an original to protect, surface a backup failure in the install output
        // instead of swallowing it and overwriting blind.
        if fileExists, !existing.isEmpty {
            let stamp = Self.backupStamp.string(from: Date())
            let backup = writeTargetURL.appendingPathExtension("bak.\(stamp)")
            do {
                try fm.copyItem(at: writeTargetURL, to: backup)
            } catch {
                FileHandle.standardError.write(Data(
                    "[beacon-hub] codex: WARNING could not back up \(writeTarget) before overwrite: \(error.localizedDescription)\n".utf8))
            }
        }

        // Codex keys the hook-trust state by fs::canonicalize(config.toml). When the file exists we
        // canonicalize IT (resolving a symlinked config.toml to its target, exactly as Codex does);
        // otherwise canonicalize the directory and append the name (a fresh regular file we create).
        let configKeyPath = Self.canonicalConfigPath(
            fileRealpath: fileExists ? realpathOrSelf(configURL.path) : nil,
            dirRealpath: realpathOrSelf(codexDir.path),
            fileName: "config.toml")

        let merged = CodexHooks.merge(existing: existing, shimCommand: codexShimInstallPath,
                                      configPath: configKeyPath)

        // If the user's config already defines any of these events via an inline array / dotted key,
        // Beacon cannot append a `[[hooks.<Event>]]` group without producing invalid TOML. Those events
        // are omitted from the written block (config stays valid); warn so the user can wire them by hand.
        if !merged.conflicts.isEmpty {
            let list = merged.conflicts.joined(separator: ", ")
            FileHandle.standardError.write(Data(
                "[beacon-hub] codex: skipped hook events already defined via inline array in config.toml: \(list). Remove those inline definitions and reinstall to enable them.\n".utf8))
        }

        // Atomic swap through any symlink (see atomicWriteThrough): no window with config.toml missing,
        // and a symlinked config.toml stays a link (we resolved it to writeTarget and write through it).
        try Self.atomicWriteThrough(target: writeTarget, content: merged.text)
    }

    // C realpath(3) on an existing path; returns the input unchanged on failure. Internal for tests.
    static func realpathOrSelf(_ path: String) -> String {
        var buf = [Int8](repeating: 0, count: Int(PATH_MAX))
        guard realpath(path, &buf) != nil else { return path }
        return String(cString: buf)
    }

    // The [hooks.state] key path Codex derives via fs::canonicalize(config.toml). If the file exists we
    // use its own canonical path (a symlinked config.toml resolves to its target); otherwise we compose
    // the canonical directory with the filename (a fresh regular file about to be created). Pure logic.
    static func canonicalConfigPath(fileRealpath: String?, dirRealpath: String, fileName: String) -> String {
        if let file = fileRealpath { return file }
        return (dirRealpath as NSString).appendingPathComponent(fileName)
    }

    // Write `content` to `target` via a same-dir temp + rename(2): atomic within the volume (never a
    // window with a missing file) and it replaces the target in place, so a symlinked target stays a
    // link (callers resolve the link and pass its target here). Preserves the target's existing mode.
    // Temp basename + error text derive from the target so failures name the real file (config.toml or
    // beacon.ts), not a hard-coded one.
    static func atomicWriteThrough(target: String, content: String) throws {
        let fm = FileManager.default
        let targetURL = URL(fileURLWithPath: target)
        let base = targetURL.lastPathComponent
        let tmp = targetURL.deletingLastPathComponent()
            .appendingPathComponent("\(base).beacon.\(UUID().uuidString)")
        try content.data(using: .utf8)!.write(to: tmp)
        if let mode = (try? fm.attributesOfItem(atPath: target))?[.posixPermissions] {
            try? fm.setAttributes([.posixPermissions: mode], ofItemAtPath: tmp.path)
        }
        guard rename(tmp.path, target) == 0 else {
            let err = String(cString: strerror(errno))
            try? fm.removeItem(at: tmp)
            throw HooksError(message: "failed to install \(base) at \(target): \(err)")
        }
    }

    private static let backupStamp: DateFormatter = {
        let f = DateFormatter(); f.dateFormat = "yyyyMMdd-HHmmss"; return f
    }()

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
