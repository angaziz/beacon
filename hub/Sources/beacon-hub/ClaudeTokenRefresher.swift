import Foundation
import Security
import Darwin
import BeaconHubKit

// Keychain access for both Claude credential blobs (#132), pulled out so the SecItem query code
// exists exactly once. The CLI's own item is read-only here -- while the `claude` CLI is installed it
// is the sole owner of its (single-use, rotating) refresh token, so beacon-hub never writes it. Beacon's
// own cache item only matters in the CLI-uninstalled world, where beacon-hub does its own OAuth refresh
// and needs somewhere durable to park the rotated token between launches.
enum ClaudeKeychain {
    private static let cliService = "Claude Code-credentials"
    private static let beaconService = "com.beacon.hub.claude-oauth"
    private static let beaconAccount = "oauth"

    static func readCLIBlob() -> Data? { read(service: cliService, account: nil) }
    static func readBeaconCacheBlob() -> Data? { read(service: beaconService, account: beaconAccount) }

    @discardableResult
    static func writeBeaconCache(_ blob: Data) -> Bool {
        let addQuery: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: beaconService,
            kSecAttrAccount as String: beaconAccount,
            kSecValueData as String: blob,
        ]
        let addStatus = SecItemAdd(addQuery as CFDictionary, nil)
        if addStatus == errSecSuccess { return true }
        guard addStatus == errSecDuplicateItem else { return false }
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: beaconService,
            kSecAttrAccount as String: beaconAccount,
        ]
        let update: [String: Any] = [kSecValueData as String: blob]
        return SecItemUpdate(query as CFDictionary, update as CFDictionary) == errSecSuccess
    }

    private static func read(service: String, account: String?) -> Data? {
        var query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        if let account { query[kSecAttrAccount as String] = account }
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess,
              let data = item as? Data
        else { return nil }
        return data
    }
}

// Executes the #132 source ladder decided by ClaudeRefreshDecision: delegates to the `claude` CLI
// (via a PTY-driven probe) when it is installed, else falls back to a direct OAuth refresh against
// Anthropic. Owns the failure-aware retry cooldown so a wedged refresh token doesn't get hammered every
// 45 s poll tick. Called from UsagePoller's background queue; internally hops to its own serial queue so
// the (up to ~12 s) delegated probe never blocks sibling providers' fetch() calls in the same poll tick.
final class ClaudeTokenRefresher {
    private let lock = NSLock()
    private var lastAttemptAt: Date?
    private var lastOutcomeFailed = false
    private let successCooldown: TimeInterval = 300
    private let failureCooldown: TimeInterval = 20

    private let session: URLSession
    private let workQueue = DispatchQueue(label: "beacon.claude-refresh")

    init(session: URLSession = .shared) { self.session = session }

    func refresh(current: ClaudeCredential, now: Date, completion: @escaping (ClaudeCredential?) -> Void) {
        workQueue.async { self.performRefresh(current: current, now: now, completion: completion) }
    }

    private func performRefresh(current: ClaudeCredential, now: Date,
                                 completion: @escaping (ClaudeCredential?) -> Void) {
        lock.lock()
        let cooldown = lastOutcomeFailed ? failureCooldown : successCooldown
        let since = lastAttemptAt.map { now.timeIntervalSince($0) }
        guard ClaudeRefreshDecision.shouldAttempt(secondsSinceLastAttempt: since, cooldown: cooldown) else {
            lock.unlock()
            completion(nil)
            return
        }
        lastAttemptAt = now
        lock.unlock()

        let cliPath = Self.resolveCLIPath()
        let path = ClaudeRefreshDecision.path(cliAvailable: cliPath != nil,
                                              refreshTokenAlive: current.refreshTokenAlive(at: now))
        log("path=\(path)")
        switch path {
        case .none:
            // Unreachable via UsagePoller's wiring (it gates on refreshTokenAlive before calling us),
            // but handled here so this type is correct standalone too.
            recordOutcome(failed: true)
            completion(nil)
        case .delegated:
            guard let cliPath else {
                recordOutcome(failed: true)
                completion(nil)
                return
            }
            let result = performDelegatedRefresh(cliPath: cliPath, current: current, now: now)
            recordOutcome(failed: result == nil)
            completion(result)
        case .direct:
            performDirectRefresh(current: current, now: now) { [self] result in
                recordOutcome(failed: result == nil)
                completion(result)
            }
        }
    }

    private func recordOutcome(failed: Bool) {
        lock.lock(); lastOutcomeFailed = failed; lock.unlock()
    }

    private func log(_ message: String) {
        FileHandle.standardError.write(Data("[beacon-hub] claude-refresh \(message)\n".utf8))
    }

    // --- CLI resolution ---

    private static let resolveLock = NSLock()
    private static var resolved = false
    private static var cachedPath: String?

    // Resolution order matches common install locations for the claude CLI (Homebrew, npm global
    // shims, the CLI's own updater dir) before falling back to a PATH lookup. Cached per process: the
    // CLI does not move mid-run, and `which` shells out.
    private static func resolveCLIPath() -> String? {
        resolveLock.lock(); defer { resolveLock.unlock() }
        if resolved { return cachedPath }
        resolved = true
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        let candidates = [
            home + "/.local/bin/claude",
            "/opt/homebrew/bin/claude",
            "/usr/local/bin/claude",
            home + "/.claude/local/claude",
        ]
        let fm = FileManager.default
        if let hit = candidates.first(where: { fm.isExecutableFile(atPath: $0) }) {
            cachedPath = hit
            return hit
        }
        let which = Process()
        which.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        which.arguments = ["which", "claude"]
        let pipe = Pipe()
        which.standardOutput = pipe
        which.standardError = Pipe()
        do {
            try which.run()
            which.waitUntilExit()
            let out = pipe.fileHandleForReading.readDataToEndOfFile()
            if which.terminationStatus == 0,
               let text = String(data: out, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines),
               !text.isEmpty, fm.isExecutableFile(atPath: text) {
                cachedPath = text
                return text
            }
        } catch {
            // env itself missing/unrunnable: no CLI reachable via PATH either.
        }
        cachedPath = nil
        return nil
    }

    // --- Delegated path: let the CLI refresh (and rewrite) its own Keychain item ---

    private func performDelegatedRefresh(cliPath: String, current: ClaudeCredential, now: Date) -> ClaudeCredential? {
        let workDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("beacon-claude-refresh-\(UUID().uuidString)")
        do {
            try FileManager.default.createDirectory(at: workDir, withIntermediateDirectories: true)
        } catch {
            log("delegated: scratch dir create failed: \(error.localizedDescription)")
            return nil
        }
        defer { cleanupProbeArtifacts(workDir: workDir) }

        var masterFD: Int32 = -1
        var slaveFD: Int32 = -1
        guard openpty(&masterFD, &slaveFD, nil, nil, nil) == 0 else {
            log("delegated: openpty failed (errno \(errno))")
            return nil
        }

        let process = Process()
        process.executableURL = URL(fileURLWithPath: cliPath)
        process.currentDirectoryURL = workDir
        process.standardInput = FileHandle(fileDescriptor: slaveFD, closeOnDealloc: false)
        process.standardOutput = FileHandle(fileDescriptor: slaveFD, closeOnDealloc: false)
        process.standardError = FileHandle(fileDescriptor: slaveFD, closeOnDealloc: false)
        var env = ProcessInfo.processInfo.environment
        env["TERM"] = "xterm-256color"
        process.environment = env

        do {
            try process.run()
        } catch {
            log("delegated: spawn failed: \(error.localizedDescription)")
            close(masterFD)
            close(slaveFD)
            return nil
        }
        // The child holds its own dup'd copies of the slave fd; the parent's copy is only needed to
        // hand off at spawn time.
        close(slaveFD)

        // The claude CLI is a TUI: continuously drain the master side so a full PTY output buffer never
        // blocks the child from writing (and thus from ever reaching a state that reads our input).
        let drainThread = Thread {
            var buf = [UInt8](repeating: 0, count: 4096)
            while true {
                let n = read(masterFD, &buf, buf.count)
                if n <= 0 { break }
            }
        }
        drainThread.start()

        func writeToMaster(_ text: String) {
            let bytes = Array(text.utf8)
            bytes.withUnsafeBufferPointer { _ = write(masterFD, $0.baseAddress, $0.count) }
        }
        func waitWhileRunning(_ timeout: TimeInterval) {
            let deadline = Date().addingTimeInterval(timeout)
            while process.isRunning && Date() < deadline {
                Thread.sleep(forTimeInterval: 0.1)
            }
        }

        Thread.sleep(forTimeInterval: 1.5)   // let the TUI finish booting before we type at it.
        writeToMaster("/status\r")
        waitWhileRunning(6.5)                 // ~8s total probe budget from spawn.
        if process.isRunning {
            writeToMaster("/exit\r")
            waitWhileRunning(1)
        }
        if process.isRunning {
            process.terminate()
            waitWhileRunning(2)
        }
        if process.isRunning {
            kill(process.processIdentifier, SIGKILL)
            waitWhileRunning(1)
        }
        close(masterFD)   // unblocks the drain thread's read() with EOF/EBADF.

        // Single re-read: each SecItemCopyMatching can prompt the user, so never poll the Keychain in
        // a loop waiting for the CLI to finish rotating.
        guard let blob = ClaudeKeychain.readCLIBlob(), let fresh = ProviderCredentials.parseClaude(blob) else {
            log("delegated: post-probe Keychain read/parse failed")
            return nil
        }
        let succeeded = fresh.accessToken != current.accessToken || !fresh.isExpired(at: Date())
        log("delegated: outcome=\(succeeded ? "ok" : "unchanged")")
        return succeeded ? fresh : nil
    }

    // Removes the scratch cwd and any Coding Buddy session the CLI probe created for it (#111): the
    // CLI namespaces sessions under ~/.claude/projects/<cwd-with-slashes-and-dots-as-dashes>, and on
    // some macOS versions the temp dir path resolves through /private, so both spellings are tried.
    private func cleanupProbeArtifacts(workDir: URL) {
        let rawPath = workDir.path
        var candidates: Set<String> = [rawPath]
        if !rawPath.hasPrefix("/private") { candidates.insert("/private" + rawPath) }
        let projectsDir = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".claude/projects")
        for path in candidates {
            let munged = path.replacingOccurrences(of: "/", with: "-").replacingOccurrences(of: ".", with: "-")
            try? FileManager.default.removeItem(at: projectsDir.appendingPathComponent(munged))
        }
        try? FileManager.default.removeItem(at: workDir)
    }

    // --- Direct path: hub-side OAuth refresh (CLI absent, so no rotating token to race) ---

    // RFC 3986 unreserved characters -- the tightest safe set for a form-urlencoded value, so the
    // (opaque, sensitive) refresh token round-trips byte-for-byte through the request body.
    private static let formValueAllowed: CharacterSet = {
        var set = CharacterSet.alphanumerics
        set.insert(charactersIn: "-._~")
        return set
    }()

    private func performDirectRefresh(current: ClaudeCredential, now: Date,
                                       completion: @escaping (ClaudeCredential?) -> Void) {
        guard let refreshToken = current.refreshToken, !refreshToken.isEmpty,
              let url = URL(string: "https://platform.claude.com/v1/oauth/token")
        else {
            completion(nil)
            return
        }
        let encoded = refreshToken.addingPercentEncoding(withAllowedCharacters: Self.formValueAllowed) ?? refreshToken
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        req.setValue("application/json", forHTTPHeaderField: "Accept")
        req.httpBody = Data("grant_type=refresh_token&refresh_token=\(encoded)&client_id=9d1c250a-e61b-44d9-88ed-5944d1962f5e".utf8)

        session.dataTask(with: req) { [self] data, resp, err in
            if let err {
                log("direct: network error: \(err.localizedDescription)")
                completion(nil)
                return
            }
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            guard code == 200, let data, let parsed = ClaudeRefreshDecision.parseRefreshResponse(data, now: now) else {
                log("direct: refresh failed (HTTP \(code))")
                completion(nil)
                return
            }
            // Rotation is not guaranteed on every call; losing the refresh token when it isn't rotated
            // would strand the user on the next expiry, so keep the current one as a fallback.
            let refreshTokenOut = parsed.refreshToken ?? refreshToken
            let blob = Self.buildKeychainBlob(accessToken: parsed.accessToken, expiresAt: parsed.expiresAt,
                                              refreshToken: refreshTokenOut,
                                              refreshTokenExpiresAt: current.refreshTokenExpiresAt)
            guard ClaudeKeychain.writeBeaconCache(blob) else {
                log("direct: Keychain cache write failed")
                completion(nil)
                return
            }
            log("direct: outcome=ok")
            completion(ClaudeCredential(accessToken: parsed.accessToken, expiresAt: parsed.expiresAt,
                                        refreshToken: refreshTokenOut,
                                        refreshTokenExpiresAt: current.refreshTokenExpiresAt))
        }.resume()
    }

    // Same claudeAiOauth shape ProviderCredentials.parseClaude expects, so Beacon's own cache item
    // round-trips through the exact same parser as the CLI's item.
    private static func buildKeychainBlob(accessToken: String, expiresAt: Date?, refreshToken: String?,
                                          refreshTokenExpiresAt: Date?) -> Data {
        var oauth: [String: Any] = ["accessToken": accessToken]
        if let expiresAt { oauth["expiresAt"] = Int64(expiresAt.timeIntervalSince1970 * 1000) }
        if let refreshToken { oauth["refreshToken"] = refreshToken }
        if let refreshTokenExpiresAt { oauth["refreshTokenExpiresAt"] = Int64(refreshTokenExpiresAt.timeIntervalSince1970 * 1000) }
        let obj: [String: Any] = ["claudeAiOauth": oauth]
        return (try? JSONSerialization.data(withJSONObject: obj)) ?? Data()
    }
}
