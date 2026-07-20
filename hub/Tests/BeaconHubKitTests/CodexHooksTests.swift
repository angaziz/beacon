import XCTest
@testable import BeaconHubKit

// Pure Codex-hooks config logic: the trust-hash computation (verified byte-exact against the real Codex
// binary), the managed [hooks] block shape, the idempotent config.toml merge, and detection.
final class CodexHooksTests: XCTestCase {

    // GOLDEN: hashes captured from codex-cli 0.140.0 `hooks/list` currentHash for the command
    // "/tmp/beacon-codex-verify/beacon-codex-hook". trustHash MUST reproduce Codex's own hash so a fresh
    // install is auto-trusted (HookTrustStatus::Trusted) and the command hook RUNS with no user step.
    func testTrustHashReproducesCodexGoldenHashes() {
        let cmd = "/tmp/beacon-codex-verify/beacon-codex-hook"
        let cases: [(label: String, timeout: Int, expected: String)] = [
            ("session_start",      600, "sha256:eeca591abcf8487ecbc4d0c15f6a064b0e634eb41d474b48d0fe4c3d764b402b"),
            ("user_prompt_submit", 600, "sha256:0ad7c573dad983b0c1204083a72981f349e8c3c23e3feb6cdb5c51fc957c7c41"),
            ("stop",               600, "sha256:0b838985fcf47caa09886f2bc361c169fa132f9be2ec9f8c4e6863b23d27decd"),
            ("permission_request", 590, "sha256:2dca6bb1f01d10266f8d7f13226c0134539bcfb5ebe397bdb14b00341e759a8b"),
        ]
        for c in cases {
            XCTAssertEqual(CodexHooks.trustHash(command: cmd, eventLabel: c.label, timeoutSec: c.timeout),
                           c.expected, "hash mismatch for \(c.label)")
        }
    }

    // A different command or timeout MUST change the hash (it is what Codex re-checks on every load).
    func testTrustHashVariesWithCommandAndTimeout() {
        let base = CodexHooks.trustHash(command: "/a/shim", eventLabel: "stop", timeoutSec: 600)
        XCTAssertNotEqual(base, CodexHooks.trustHash(command: "/b/shim", eventLabel: "stop", timeoutSec: 600))
        XCTAssertNotEqual(base, CodexHooks.trustHash(command: "/a/shim", eventLabel: "stop", timeoutSec: 590))
    }

    func testStateKeyFormat() {
        XCTAssertEqual(CodexHooks.stateKey(configPath: "/Users/me/.codex/config.toml", eventLabel: "session_start"),
                       "/Users/me/.codex/config.toml:session_start:0:0")
    }

    // The managed block wires all five events; only PermissionRequest pins timeout=590; every event gets
    // a trusted_hash state entry keyed by the config path.
    func testManagedBlockShape() {
        let shim = "/Users/me/.beacon/beacon-codex-hook"
        let cfg = "/Users/me/.codex/config.toml"
        let block = CodexHooks.managedBlock(shimCommand: shim, configPath: cfg)
        for ev in ["SessionStart", "UserPromptSubmit", "Stop", "SessionEnd", "PermissionRequest"] {
            XCTAssertTrue(block.contains("[[hooks.\(ev)]]"), "missing [[hooks.\(ev)]]")
        }
        XCTAssertTrue(block.contains("timeout = 590"), "PermissionRequest must pin the 590s timeout")
        XCTAssertEqual(block.components(separatedBy: "timeout =").count - 1, 1,
                       "only PermissionRequest carries an explicit timeout")
        XCTAssertTrue(block.contains("[hooks.state]"))
        for label in ["session_start", "user_prompt_submit", "stop", "session_end", "permission_request"] {
            let key = CodexHooks.stateKey(configPath: cfg, eventLabel: label)
            XCTAssertTrue(block.contains("\"\(key)\" = { enabled = true, trusted_hash = "),
                          "missing trust state for \(label)")
        }
        XCTAssertTrue(block.hasPrefix(CodexHooks.beginMarker))
        XCTAssertTrue(block.hasSuffix(CodexHooks.endMarker))
    }

    // Merge preserves unrelated content and is idempotent (re-running yields exactly one managed block).
    func testMergePreservesAndIsIdempotent() {
        let shim = "/Users/me/.beacon/beacon-codex-hook"
        let cfg = "/Users/me/.codex/config.toml"
        let existing = "model = \"o3\"\n\n[tui]\nnotifications = true\n"
        let once = CodexHooks.merge(existing: existing, shimCommand: shim, configPath: cfg).text
        XCTAssertTrue(once.contains("model = \"o3\""), "unrelated content must survive")
        XCTAssertTrue(once.contains("notifications = true"))
        XCTAssertTrue(once.contains(CodexHooks.beginMarker))

        let twice = CodexHooks.merge(existing: once, shimCommand: shim, configPath: cfg).text
        XCTAssertEqual(twice.components(separatedBy: CodexHooks.beginMarker).count - 1, 1,
                       "re-install must leave exactly one managed block")
        XCTAssertEqual(once, twice, "merge must be idempotent")
        XCTAssertTrue(twice.contains("model = \"o3\""))
    }

    // Merge into an empty config yields just the block (plus trailing newline).
    func testMergeEmptyConfig() {
        let out = CodexHooks.merge(existing: "", shimCommand: "/s", configPath: "/c/config.toml").text
        XCTAssertTrue(out.hasPrefix(CodexHooks.beginMarker))
        XCTAssertEqual(out.components(separatedBy: CodexHooks.beginMarker).count - 1, 1)
    }

    func testIsInstalledDetection() {
        let shim = "/Users/me/.beacon/beacon-codex-hook"
        let cfg = "/Users/me/.codex/config.toml"
        let installed = CodexHooks.merge(existing: "model = \"o3\"\n", shimCommand: shim, configPath: cfg).text
        XCTAssertTrue(CodexHooks.isInstalled(configText: installed, shimCommand: shim))
        // A block wiring a DIFFERENT shim command does not count as installed for this shim.
        XCTAssertFalse(CodexHooks.isInstalled(configText: installed, shimCommand: "/other/shim"))
        // No managed block at all.
        XCTAssertFalse(CodexHooks.isInstalled(configText: "model = \"o3\"\n", shimCommand: shim))
    }

    // --- state placement (avoid a duplicate [hooks.state] table when the user already has one) ---

    // Count whole-line [hooks.state] table headers (the TOML redefinition hazard we must never emit).
    private func stateHeaderCount(_ s: String) -> Int {
        s.components(separatedBy: "\n").filter(CodexHooks.isStateHeaderLine).count
    }

    // Every Beacon-written state line carries the trailing marker so it is strippable anywhere.
    private func beaconStateLines(_ s: String) -> [String] {
        s.components(separatedBy: "\n").filter { $0.hasSuffix(CodexHooks.stateMarker) }
    }

    // A pre-existing [hooks.state] with user entries: beacon lines go AFTER the header, user entries stay
    // verbatim, and the output carries exactly one [hooks.state] header. Re-merge is byte-idempotent.
    func testMergeInsertsIntoExistingUserStateTable() {
        let shim = "/Users/me/.beacon/beacon-codex-hook"
        let cfg = "/Users/me/.codex/config.toml"
        let userEntry = "\"/other/hook:stop:0:0\" = { enabled = true, trusted_hash = \"sha256:user\" }"
        let existing = "model = \"o3\"\n\n[hooks.state]\n\(userEntry)\n"

        let once = CodexHooks.merge(existing: existing, shimCommand: shim, configPath: cfg).text
        XCTAssertEqual(stateHeaderCount(once), 1, "must not add a second [hooks.state] header")
        XCTAssertTrue(once.contains(userEntry), "user state entry must survive verbatim")
        XCTAssertEqual(beaconStateLines(once).count, 5, "all five marked beacon state lines present")
        XCTAssertFalse(once.contains("[hooks.state]\n" + CodexHooks.beginMarker),
                       "the managed block must not carry its own [hooks.state] in this branch")
        // Beacon lines land immediately after the single header line, before the user entry.
        let lines = once.components(separatedBy: "\n")
        let header = lines.firstIndex(where: CodexHooks.isStateHeaderLine)!
        for i in 1...5 { XCTAssertTrue(lines[header + i].hasSuffix(CodexHooks.stateMarker)) }

        let twice = CodexHooks.merge(existing: once, shimCommand: shim, configPath: cfg).text
        XCTAssertEqual(once, twice, "re-merge into an existing table must be byte-idempotent")
        XCTAssertEqual(stateHeaderCount(twice), 1)
    }

    // A fresh config (no [hooks.state]) gets the self-contained block: its own header + five marked lines.
    // Re-merge is byte-idempotent.
    func testMergeFreshConfigSelfContainedAndIdempotent() {
        let shim = "/Users/me/.beacon/beacon-codex-hook"
        let cfg = "/Users/me/.codex/config.toml"
        let once = CodexHooks.merge(existing: "model = \"o3\"\n", shimCommand: shim, configPath: cfg).text
        XCTAssertEqual(stateHeaderCount(once), 1, "block carries its own [hooks.state]")
        XCTAssertEqual(beaconStateLines(once).count, 5)
        // Its own [hooks.state] lives inside the managed block.
        let stateInBlock = once.contains(CodexHooks.beginMarker) && once.range(of: "[hooks.state]") != nil
        XCTAssertTrue(stateInBlock)
        let twice = CodexHooks.merge(existing: once, shimCommand: shim, configPath: cfg).text
        XCTAssertEqual(once, twice, "fresh-branch merge must be byte-idempotent")
    }

    // strip removes only Beacon-marked lines and the managed block; unmarked user lines survive.
    func testStripRemovesOnlyBeaconMarkedLines() {
        let shim = "/s"
        let cfg = "/c/config.toml"
        let userEntry = "\"/other:stop:0:0\" = { enabled = true, trusted_hash = \"sha256:user\" }"
        let existing = "model = \"o3\"\n\n[hooks.state]\n\(userEntry)\n"
        let merged = CodexHooks.merge(existing: existing, shimCommand: shim, configPath: cfg).text
        let stripped = CodexHooks.stripManaged(merged)
        XCTAssertEqual(beaconStateLines(stripped).count, 0, "no beacon-marked lines remain after strip")
        XCTAssertFalse(stripped.contains(CodexHooks.beginMarker), "managed block removed")
        XCTAssertTrue(stripped.contains(userEntry), "unmarked user entry preserved")
        XCTAssertTrue(stripped.contains("model = \"o3\""))
    }

    // Upgrade path: a config from the OLD merge format (own [hooks.state] INSIDE the managed block) whose
    // user later added their OWN [hooks.state] elsewhere (two tables => invalid TOML). Re-merge must heal
    // it: the block's own state is stripped with the block, beacon lines move under the user's header, and
    // the output ends with exactly one [hooks.state] header.
    func testUpgradeFromOldFormatWithUserStateHeals() {
        let shim = "/Users/me/.beacon/beacon-codex-hook"
        let cfg = "/Users/me/.codex/config.toml"
        let userEntry = "\"/user/hook:stop:0:0\" = { enabled = true, trusted_hash = \"sha256:user\" }"
        // Old-format block: unmarked state lines inside its own [hooks.state], all within the markers.
        let oldBlock = [
            CodexHooks.beginMarker,
            "[[hooks.SessionStart]]",
            "hooks = [{ type = \"command\", command = \"\(shim)\" }]",
            "",
            "[hooks.state]",
            "\"\(CodexHooks.stateKey(configPath: cfg, eventLabel: "session_start"))\" = { enabled = true, trusted_hash = \"sha256:old\" }",
            CodexHooks.endMarker,
        ].joined(separator: "\n")
        let existing = "model = \"o3\"\n\n[hooks.state]\n\(userEntry)\n\n\(oldBlock)\n"
        XCTAssertEqual(stateHeaderCount(existing), 2, "precondition: the old+user combo is a double table")

        let out = CodexHooks.merge(existing: existing, shimCommand: shim, configPath: cfg).text
        XCTAssertEqual(stateHeaderCount(out), 1, "healed to a single [hooks.state] header")
        XCTAssertTrue(out.contains(userEntry), "user entry preserved")
        XCTAssertFalse(out.contains("sha256:old"), "stale old-format state dropped with the block")
        XCTAssertEqual(beaconStateLines(out).count, 5)
        let twice = CodexHooks.merge(existing: out, shimCommand: shim, configPath: cfg).text
        XCTAssertEqual(out, twice, "healed output is idempotent")
    }

    // Validity guard: merge NEVER emits two [hooks.state] header lines, across placements and re-merges.
    func testMergeNeverEmitsTwoStateHeaders() {
        let shim = "/s"
        let cfg = "/c/config.toml"
        let inputs = [
            "",
            "model = \"o3\"\n",
            "model = \"o3\"\n\n[hooks.state]\n\"/u:stop:0:0\" = { enabled = true, trusted_hash = \"sha256:u\" }\n",
            "  [hooks.state]  # my table\n\"/u:stop:0:0\" = { enabled = true, trusted_hash = \"sha256:u\" }\n",
        ]
        for input in inputs {
            let once = CodexHooks.merge(existing: input, shimCommand: shim, configPath: cfg).text
            XCTAssertLessThanOrEqual(stateHeaderCount(once), 1, "single header for input: \(input.debugDescription)")
            let twice = CodexHooks.merge(existing: once, shimCommand: shim, configPath: cfg).text
            XCTAssertLessThanOrEqual(stateHeaderCount(twice), 1)
            XCTAssertEqual(once, twice, "idempotent for input: \(input.debugDescription)")
        }
    }

    // --- file-order group index (Codex keys trust by the event's matcher-group index across the config) ---

    // Extract the group index of a state key line for a given event label from merged output.
    private func stateGroupIndex(_ s: String, cfg: String, label: String) -> Int? {
        let prefix = "\"\(cfg):\(label):"
        for line in s.components(separatedBy: "\n") where line.hasPrefix(prefix) {
            let rest = line.dropFirst(prefix.count)               // "<group>:<handler>" = { ... }
            let group = rest.prefix { $0 != ":" }
            return Int(group)
        }
        return nil
    }

    // One pre-existing user [[hooks.PermissionRequest]] group: Beacon's appended group is index 1 for that
    // event; all untouched events stay at index 0. Handler index stays 0.
    func testGroupIndexAccountsForPreExistingUserGroup() {
        let shim = "/Users/me/.beacon/beacon-codex-hook"
        let cfg = "/Users/me/.codex/config.toml"
        let existing = """
        [[hooks.PermissionRequest]]
        hooks = [{ type = "command", command = "/user/other" }]

        """
        let out = CodexHooks.merge(existing: existing, shimCommand: shim, configPath: cfg).text
        XCTAssertEqual(stateGroupIndex(out, cfg: cfg, label: "permission_request"), 1,
                       "Beacon's PermissionRequest group lands after the user's group (index 1)")
        for label in ["session_start", "user_prompt_submit", "stop", "session_end"] {
            XCTAssertEqual(stateGroupIndex(out, cfg: cfg, label: label), 0, "untouched event stays at 0")
        }
        XCTAssertTrue(out.contains("\"\(cfg):permission_request:1:0\" = { enabled = true, trusted_hash = "))
        // Idempotent: strip removes only Beacon content, so the recomputed index is stable.
        let twice = CodexHooks.merge(existing: out, shimCommand: shim, configPath: cfg).text
        XCTAssertEqual(out, twice, "group-index merge must be byte-idempotent")
    }

    // Multiple pre-existing groups for the same event -> Beacon's index is their count.
    func testGroupIndexCountsMultiplePreExistingGroups() {
        let shim = "/s"
        let cfg = "/c/config.toml"
        let existing = """
        [[hooks.Stop]]
        hooks = [{ type = "command", command = "/a" }]
        [[hooks.Stop]]
        hooks = [{ type = "command", command = "/b" }]
        [[hooks.Stop]]
        hooks = [{ type = "command", command = "/c" }]

        """
        let out = CodexHooks.merge(existing: existing, shimCommand: shim, configPath: cfg).text
        XCTAssertEqual(stateGroupIndex(out, cfg: cfg, label: "stop"), 3, "three user groups -> Beacon at index 3")
        XCTAssertEqual(CodexHooks.groupCount(in: existing, eventToml: "Stop"), 3)
    }

    // Fresh config: no pre-existing groups -> every event at group index 0 (regression guard on the default).
    func testGroupIndexZeroOnFreshConfig() {
        let cfg = "/c/config.toml"
        let out = CodexHooks.merge(existing: "model = \"o3\"\n", shimCommand: "/s", configPath: cfg).text
        for label in ["session_start", "user_prompt_submit", "stop", "session_end", "permission_request"] {
            XCTAssertEqual(stateGroupIndex(out, cfg: cfg, label: label), 0)
        }
    }

    // --- inline-array / dotted-key conflicts (appending [[hooks.<Event>]] there is invalid TOML) ---

    // A `[hooks]` table with an inline `PermissionRequest = [...]` array: that event is reported as a
    // conflict, its group/state are omitted, the other four events install, and no broken group is written.
    func testInlineArrayUnderHooksTableIsReportedNotCorrupted() {
        let shim = "/s"
        let cfg = "/c/config.toml"
        let existing = """
        [hooks]
        PermissionRequest = [{ type = "command", command = "/user/inline" }]

        """
        let result = CodexHooks.merge(existing: existing, shimCommand: shim, configPath: cfg)
        XCTAssertEqual(result.conflicts, ["PermissionRequest"], "the inline-array event is reported")
        XCTAssertFalse(result.text.contains("[[hooks.PermissionRequest]]"),
                       "must NOT append an array-of-tables that redefines the inline array")
        XCTAssertNil(stateGroupIndex(result.text, cfg: cfg, label: "permission_request"),
                     "no state entry for the conflicted event")
        XCTAssertTrue(result.text.contains("[[hooks.SessionStart]]"), "other events still install")
        XCTAssertTrue(result.text.contains("PermissionRequest = [{ type = \"command\""),
                      "user's inline array preserved verbatim")
        // Idempotent, conflict stable.
        let twice = CodexHooks.merge(existing: result.text, shimCommand: shim, configPath: cfg)
        XCTAssertEqual(result.text, twice.text)
        XCTAssertEqual(twice.conflicts, ["PermissionRequest"])
    }

    // A dotted `hooks.Stop = [...]` assignment is likewise a conflict.
    func testDottedKeyAssignmentIsReported() {
        let shim = "/s"
        let cfg = "/c/config.toml"
        let existing = "hooks.Stop = [{ type = \"command\", command = \"/user/inline\" }]\n"
        let result = CodexHooks.merge(existing: existing, shimCommand: shim, configPath: cfg)
        XCTAssertEqual(result.conflicts, ["Stop"])
        XCTAssertFalse(result.text.contains("[[hooks.Stop]]"))
        XCTAssertTrue(result.text.contains("[[hooks.SessionStart]]"), "non-conflicting events install")
    }

    // A pre-existing `[[hooks.<Event>]]` array-of-tables is NOT a conflict (it is extensible); it only
    // bumps the group index.
    func testArrayOfTablesIsNotAConflict() {
        let shim = "/s"
        let cfg = "/c/config.toml"
        let existing = "[[hooks.Stop]]\nhooks = [{ type = \"command\", command = \"/user\" }]\n"
        let result = CodexHooks.merge(existing: existing, shimCommand: shim, configPath: cfg)
        XCTAssertTrue(result.conflicts.isEmpty, "array-of-tables is extensible, not a conflict")
        XCTAssertEqual(stateGroupIndex(result.text, cfg: cfg, label: "stop"), 1)
    }
}
