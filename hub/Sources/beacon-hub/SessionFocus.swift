import Foundation

// Tap-to-open focus resolver (issue #110, P2-b). Split into a pure planner (unit-tested) and an
// executor so the decision logic can be verified without spawning processes.

struct FocusTarget: Equatable {
    let hostApp: String?
    let focusURL: String?
    let bundleId: String?
    let cwd: String
}

enum FocusAction: Equatable {
    case url(String)            // Tier 1: Warp focus_url => `open <url>`
    case editorReuse(cwd: String)  // Tier 2: VSCode/Cursor => `cursor -r <cwd>` (or `code -r`)
    case openBundle(String)     // Tier 3a: known bundleId => `/usr/bin/open -b <bundle>`
    case openApp(String)        // Tier 3b: mapped app name => `/usr/bin/open -a <name>`
    case none
}

enum SessionFocus {

    // Pure tier resolver: no side-effects, unit-testable.
    static func plan(_ t: FocusTarget) -> FocusAction {
        if let u = t.focusURL, !u.isEmpty { return .url(u) }
        if let a = t.hostApp, a == "vscode" || a == "Cursor" { return .editorReuse(cwd: t.cwd) }
        if let b = t.bundleId, !b.isEmpty { return .openBundle(b) }
        if let a = t.hostApp, !a.isEmpty { return .openApp(appName(for: a)) }
        return .none
    }

    // Maps TERM_PROGRAM / terminal identifiers to macOS app names.
    static func appName(for termProgram: String) -> String {
        switch termProgram {
        case "WarpTerminal":   return "Warp"
        case "ghostty":        return "Ghostty"
        case "Apple_Terminal": return "Terminal"
        case "iTerm.app":      return "iTerm"
        default:
            // Strip ".app" suffix if present; otherwise pass through (best-effort).
            return termProgram.hasSuffix(".app")
                ? String(termProgram.dropLast(4))
                : termProgram
        }
    }

    // Execute the resolved action. Must be called off the main thread by the caller (it may block
    // briefly on process launch). Returns true if the action was dispatched without known failure.
    // `.none` always returns false. Never hangs: each Process call is fire-and-forget for url/app/bundle
    // (open(1) returns immediately), or gets a short wait cap for editor CLI reuse.
    @discardableResult
    static func focus(_ t: FocusTarget) -> Bool {
        switch plan(t) {
        case .url(let u):
            return run("/usr/bin/open", [u])
        case .editorReuse(let cwd):
            // Prefer `cursor`, fall back to `code`, then open by bundle if we have one.
            if let cursor = editorCLI("cursor") { return run(cursor, ["-r", cwd], waitCap: 5) }
            if let code = editorCLI("code")     { return run(code,   ["-r", cwd], waitCap: 5) }
            // Neither CLI found: fall back to open-by-bundle if available.
            if let b = t.bundleId, !b.isEmpty   { return run("/usr/bin/open", ["-b", b]) }
            return false
        case .openBundle(let b):
            return run("/usr/bin/open", ["-b", b])
        case .openApp(let name):
            return run("/usr/bin/open", ["-a", name])
        case .none:
            return false
        }
    }

    // Resolve an editor CLI path. Checks well-known install locations then PATH via /usr/bin/env.
    private static func editorCLI(_ name: String) -> String? {
        let knownPaths = [
            "\(NSHomeDirectory())/.local/bin/\(name)",
            "/usr/local/bin/\(name)",
            "/opt/homebrew/bin/\(name)",
        ]
        for p in knownPaths where FileManager.default.isExecutableFile(atPath: p) { return p }
        // PATH fallback: use `which` rather than env-exec so we get the path without launching the editor.
        let result = shell("/usr/bin/which", [name])
        let trimmed = result?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return trimmed.isEmpty ? nil : trimmed
    }

    // Run a command, optionally waiting at most `waitCap` seconds before detaching.
    // Returns true if the process launched without error.
    @discardableResult
    private static func run(_ exe: String, _ args: [String], waitCap: TimeInterval? = nil) -> Bool {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: exe)
        p.arguments = args
        do {
            try p.run()
            if let cap = waitCap {
                // Time-boxed wait: detach if it takes longer than expected (avoids hangs).
                DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + cap) {
                    if p.isRunning { p.terminate() }
                }
                p.waitUntilExit()
            }
            return true
        } catch {
            return false
        }
    }

    // Thin wrapper: run a command and capture stdout as a String.
    private static func shell(_ exe: String, _ args: [String]) -> String? {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: exe)
        p.arguments = args
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = Pipe()   // suppress stderr
        do {
            try p.run()
            p.waitUntilExit()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            return String(data: data, encoding: .utf8)
        } catch {
            return nil
        }
    }
}
