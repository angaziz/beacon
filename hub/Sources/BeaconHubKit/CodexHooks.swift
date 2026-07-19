import Foundation
import CryptoKit

// Pure Codex-hooks config logic (design 2026-07-19), split out of the executable so the TOML block
// generation, the trust-hash computation, and the idempotent merge are host-tested. The executable's
// HooksInstaller only does file IO (copy the shim, read/write ~/.codex/config.toml) around this.
//
// TRUST (verified against openai/codex codex-rs/hooks discovery @ 0fb559f0 and codex-cli 0.140.0):
// a user-config command hook only RUNS when its persisted `[hooks.state]` trusted_hash equals the hash
// Codex derives for it -- otherwise it is discovered but never dispatched (HookTrustStatus::Untrusted).
// Codex derives the hash as sha256 over the CANONICAL compact JSON of a normalized identity
//   {"event_name":"<label>","hooks":[{"async":false,"command":"<cmd>","timeout":<normalized>,"type":"command"}]}
// (object keys sorted; matcher omitted when unset). We reproduce that byte-for-byte and write the state,
// so a fresh install is immediately trusted -- no interactive trust step. The state key is
//   "<canonicalized config.toml path>:<event label>:<group index>:<handler index>"
// (group/handler are 0 because we write exactly one group with one hook per event).
public enum CodexHooks {

    // The LocalIngestServer route the beacon-codex-hook shim POSTs to.
    public static let routePath = "/codex/hook"

    // Managed-block markers in ~/.codex/config.toml. Everything between them (inclusive) is Beacon-owned
    // and rewritten on every (re)install; unrelated config is preserved verbatim.
    public static let beginMarker = "# >>> beacon-codex-hooks (managed by Beacon Hub; do not edit) >>>"
    public static let endMarker   = "# <<< beacon-codex-hooks (managed by Beacon Hub; do not edit) <<<"

    // Trailing comment marker on every Beacon-written [hooks.state] entry line. A trailing comment after
    // an inline table is valid TOML, so a marked entry stays identifiable wherever it lives -- inside the
    // managed block's own [hooks.state] (fresh config) OR inserted after a user's pre-existing
    // [hooks.state] header. merge strips marked lines anywhere, keeping re-install idempotent.
    public static let stateMarker = "# beacon:managed"

    // One managed hook. `hashTimeout` is the timeout Codex uses AFTER normalization (config default ->
    // 600 for most events, 1 for SessionEnd; PermissionRequest pins 590). `configTimeout` is what we
    // write into config.toml (nil => omit, letting Codex apply its default, which we mirror in the hash).
    public struct Event: Equatable {
        public let toml: String        // TOML event header (e.g. "SessionStart")
        public let label: String       // persisted-state event label (e.g. "session_start")
        public let hashTimeout: Int     // timeout Codex hashes (post-normalization)
        public let configTimeout: Int?  // timeout written to config.toml (nil => omitted)
    }

    // The five events the buddy adapter bridges. Order is the config write order (group indices are per
    // event, so all are group 0). Timeouts verified byte-exact against codex 0.140.0 hooks/list.
    public static let events: [Event] = [
        Event(toml: "SessionStart",      label: "session_start",      hashTimeout: 600, configTimeout: nil),
        Event(toml: "UserPromptSubmit",  label: "user_prompt_submit", hashTimeout: 600, configTimeout: nil),
        Event(toml: "Stop",              label: "stop",               hashTimeout: 600, configTimeout: nil),
        Event(toml: "SessionEnd",        label: "session_end",        hashTimeout: 1,   configTimeout: nil),
        Event(toml: "PermissionRequest", label: "permission_request", hashTimeout: 590, configTimeout: 590),
    ]

    // sha256 over the canonical compact JSON identity Codex hashes for a command hook. `command` is the
    // literal command string as written in config.toml.
    public static func trustHash(command: String, eventLabel: String, timeoutSec: Int) -> String {
        let handler = "{\"async\":false,\"command\":\(jsonString(command)),\"timeout\":\(timeoutSec),\"type\":\"command\"}"
        let identity = "{\"event_name\":\(jsonString(eventLabel)),\"hooks\":[\(handler)]}"
        let digest = SHA256.hash(data: Data(identity.utf8))
        let hex = digest.map { String(format: "%02x", $0) }.joined()
        return "sha256:\(hex)"
    }

    // Result of a merge: the config text to write, plus the events Beacon could NOT safely wire because
    // the user's config already defines them via an inline array / dotted assignment (extending those
    // with `[[hooks.<Event>]]` is invalid TOML). Conflicted events are omitted from the written block so
    // the config stays valid; the caller surfaces `conflicts` to the user.
    public struct MergeResult: Equatable {
        public let text: String
        public let conflicts: [String]   // event TOML names (e.g. "PermissionRequest"), in `events` order
    }

    // The persisted-state key Codex builds per handler: "<config path>:<label>:<group>:<handler>". `group`
    // is the event's matcher-group index in FILE ORDER across the whole config (Beacon's appended group
    // lands after any pre-existing user groups for that event); `handler` is 0 (one hook per group).
    public static func stateKey(configPath: String, eventLabel: String, groupIndex: Int = 0) -> String {
        "\(configPath):\(eventLabel):\(groupIndex):0"
    }

    // The marked [hooks.state] entry lines trusting each managed hook. Each carries the trailing
    // stateMarker so merge can find and replace it wherever it lives. `groupIndices` maps an event's TOML
    // name to its computed file-order group index (absent => 0, the fresh-config case).
    static func stateLines(shimCommand: String, configPath: String, events evs: [Event],
                           groupIndices: [String: Int]) -> [String] {
        evs.map { e in
            let key = stateKey(configPath: configPath, eventLabel: e.label, groupIndex: groupIndices[e.toml] ?? 0)
            let hash = trustHash(command: shimCommand, eventLabel: e.label, timeoutSec: e.hashTimeout)
            return "\(tomlString(key)) = { enabled = true, trusted_hash = \(tomlString(hash)) } \(stateMarker)"
        }
    }

    // The full managed block: one [[hooks.<Event>]] group per event wiring the shim, plus (when
    // `includeState`) a [hooks.state] table trusting each. `shimCommand` is the literal command path;
    // `configPath` is the CANONICALIZED ~/.codex/config.toml path (symlinks resolved, as Codex does via
    // fs::canonicalize). The public no-context signature wires all five events at group index 0 (the
    // fresh-install path); merge uses the internal overload with the usable-event subset + computed indices.
    public static func managedBlock(shimCommand: String, configPath: String) -> String {
        managedBlock(shimCommand: shimCommand, configPath: configPath, includeState: true,
                     events: events, groupIndices: [:])
    }

    static func managedBlock(shimCommand: String, configPath: String, includeState: Bool,
                             events evs: [Event], groupIndices: [String: Int]) -> String {
        var lines: [String] = [beginMarker]
        for e in evs {
            lines.append("[[hooks.\(e.toml)]]")
            if let t = e.configTimeout {
                lines.append("hooks = [{ type = \"command\", command = \(tomlString(shimCommand)), timeout = \(t) }]")
            } else {
                lines.append("hooks = [{ type = \"command\", command = \(tomlString(shimCommand)) }]")
            }
            lines.append("")
        }
        if includeState {
            lines.append("[hooks.state]")
            lines.append(contentsOf: stateLines(shimCommand: shimCommand, configPath: configPath,
                                                 events: evs, groupIndices: groupIndices))
        } else if lines.last == "" {
            lines.removeLast()  // drop the blank left after the last group so the block ends at endMarker
        }
        lines.append(endMarker)
        return lines.joined(separator: "\n")
    }

    // Idempotent merge. Strip any prior managed block AND any Beacon-marked state line anywhere, then:
    //  - skip any event the remaining config defines via an inline array / dotted assignment (reported in
    //    `conflicts`; appending `[[hooks.<Event>]]` there would be invalid TOML);
    //  - for each remaining event, compute its file-order group index = count of pre-existing user groups
    //    for that event, so the written trusted_hash key matches what Codex derives;
    //  - if the config still has a [hooks.state] header, insert the marked state lines after it and append
    //    the block WITHOUT its own state header; otherwise append the self-contained block.
    // Byte-idempotent on its own output (strip removes only Beacon content, so indices/conflicts are stable).
    public static func merge(existing: String, shimCommand: String, configPath: String) -> MergeResult {
        let base = stripManaged(existing)
        let conflicts = events.map(\.toml).filter { inlineArrayConflict(in: base, eventToml: $0) }
        let usable = events.filter { !conflicts.contains($0.toml) }
        var groupIndices: [String: Int] = [:]
        for e in usable { groupIndices[e.toml] = groupCount(in: base, eventToml: e.toml) }

        // Nothing safely installable (every event conflicts): leave the user's config untouched.
        if usable.isEmpty {
            return MergeResult(text: base.isEmpty ? "" : base + "\n", conflicts: conflicts)
        }

        if let idx = firstStateHeaderIndex(base) {
            var lines = base.components(separatedBy: "\n")
            lines.insert(contentsOf: stateLines(shimCommand: shimCommand, configPath: configPath,
                                                 events: usable, groupIndices: groupIndices), at: idx + 1)
            let block = managedBlock(shimCommand: shimCommand, configPath: configPath, includeState: false,
                                     events: usable, groupIndices: groupIndices)
            return MergeResult(text: lines.joined(separator: "\n") + "\n\n" + block + "\n", conflicts: conflicts)
        }
        let block = managedBlock(shimCommand: shimCommand, configPath: configPath, includeState: true,
                                 events: usable, groupIndices: groupIndices)
        let text = base.isEmpty ? block + "\n" : base + "\n\n" + block + "\n"
        return MergeResult(text: text, conflicts: conflicts)
    }

    // True iff the config already carries a managed block whose shim command matches. Cheap substring
    // check (no TOML parse); powers reinstall no-op detection and the menubar Codex hook state.
    public static func isInstalled(configText: String, shimCommand: String) -> Bool {
        guard let block = extractManagedBlock(configText) else { return false }
        return block.contains(tomlString(shimCommand))
    }

    // --- helpers ---

    // Index of the first `[hooks.state]` table header line (optional surrounding whitespace + optional
    // trailing comment), or nil. Used to place marked state lines after a user's existing table.
    static func firstStateHeaderIndex(_ text: String) -> Int? {
        text.components(separatedBy: "\n").firstIndex(where: isStateHeaderLine)
    }

    static func isStateHeaderLine(_ line: String) -> Bool {
        var s = line.trimmingCharacters(in: .whitespaces)
        guard s.hasPrefix("[hooks.state]") else { return false }
        s.removeFirst("[hooks.state]".count)
        s = s.trimmingCharacters(in: .whitespaces)
        return s.isEmpty || s.hasPrefix("#")
    }

    // Count whole-line `[[hooks.<Event>]]` array-of-tables headers (optional surrounding whitespace +
    // optional trailing comment) for one event. This is Beacon's appended group index for that event.
    static func groupCount(in text: String, eventToml: String) -> Int {
        let header = "[[hooks.\(eventToml)]]"
        return text.components(separatedBy: "\n").filter { line in
            var s = line.trimmingCharacters(in: .whitespaces)
            guard s.hasPrefix(header) else { return false }
            s.removeFirst(header.count)
            s = s.trimmingCharacters(in: .whitespaces)
            return s.isEmpty || s.hasPrefix("#")
        }.count
    }

    // True iff the config defines `<Event>` via a value assignment (an inline array under [hooks], or a
    // `hooks.<Event> = ...` dotted key). Appending `[[hooks.<Event>]]` to such a config is invalid TOML
    // (cannot extend a value with an array-of-tables), so merge omits and reports the event instead.
    static func inlineArrayConflict(in text: String, eventToml: String) -> Bool {
        var currentTable = ""
        for raw in text.components(separatedBy: "\n") {
            let line = stripInlineComment(raw).trimmingCharacters(in: .whitespaces)
            if line.isEmpty { continue }
            if line.hasPrefix("[[") && line.hasSuffix("]]") {
                currentTable = String(line.dropFirst(2).dropLast(2)).trimmingCharacters(in: .whitespaces)
                continue
            }
            if line.hasPrefix("[") && line.hasSuffix("]") {
                currentTable = String(line.dropFirst().dropLast()).trimmingCharacters(in: .whitespaces)
                continue
            }
            guard let eq = line.firstIndex(of: "=") else { continue }
            let key = line[..<eq].trimmingCharacters(in: .whitespaces)
            if key == "hooks.\(eventToml)" { return true }
            if currentTable == "hooks" && key == eventToml { return true }
        }
        return false
    }

    // Drop an unquoted trailing `# ...` comment. Adequate for the header/key parsing above (TOML table
    // names and bare keys never contain `#`).
    static func stripInlineComment(_ line: String) -> String {
        guard let h = line.firstIndex(of: "#") else { return line }
        return String(line[..<h])
    }

    // Remove the managed block (markers inclusive), every Beacon-marked state line anywhere, and any
    // leading/trailing blank lines that leaves behind; keep all other content verbatim. Returns "" when
    // nothing but the block (and whitespace) remained.
    static func stripManaged(_ text: String) -> String {
        var out: [String] = []
        var inBlock = false
        for line in text.components(separatedBy: "\n") {
            if line == beginMarker { inBlock = true; continue }
            if line == endMarker { inBlock = false; continue }
            if inBlock { continue }
            if line.trimmingCharacters(in: .whitespaces).hasSuffix(stateMarker) { continue }
            out.append(line)
        }
        while let last = out.last, last.trimmingCharacters(in: .whitespaces).isEmpty { out.removeLast() }
        while let first = out.first, first.trimmingCharacters(in: .whitespaces).isEmpty { out.removeFirst() }
        return out.joined(separator: "\n")
    }

    static func extractManagedBlock(_ text: String) -> String? {
        let lines = text.components(separatedBy: "\n")
        guard let b = lines.firstIndex(of: beginMarker),
              let e = lines.firstIndex(of: endMarker), b < e else { return nil }
        return lines[b...e].joined(separator: "\n")
    }

    // serde_json string escaping (matches Codex's canonical JSON): escape ", \, and C0 control chars;
    // pass non-ASCII through as UTF-8. Sufficient and verified for filesystem-path commands.
    static func jsonString(_ s: String) -> String {
        var out = "\""
        for c in s.unicodeScalars {
            switch c {
            case "\"": out += "\\\""
            case "\\": out += "\\\\"
            case "\u{08}": out += "\\b"
            case "\u{0C}": out += "\\f"
            case "\n": out += "\\n"
            case "\r": out += "\\r"
            case "\t": out += "\\t"
            default:
                if c.value < 0x20 { out += String(format: "\\u%04x", c.value) }
                else { out.unicodeScalars.append(c) }
            }
        }
        out += "\""
        return out
    }

    // TOML basic string: escape " and \. Filesystem paths need nothing more.
    static func tomlString(_ s: String) -> String {
        var out = "\""
        for c in s.unicodeScalars {
            switch c {
            case "\"": out += "\\\""
            case "\\": out += "\\\\"
            default: out.unicodeScalars.append(c)
            }
        }
        out += "\""
        return out
    }
}
