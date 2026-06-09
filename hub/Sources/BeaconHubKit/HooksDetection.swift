import Foundation

// Pure dict -> Bool detection of the beacon Claude Code hooks, split out of the executable's
// HooksInstaller so it is unit-testable without touching real $HOME (the file read + Process stay in
// the executable). "Installed" is intentionally STRICT: the optional SessionStart/Stop/... hooks can
// be present while the round-trip-essential PermissionRequest hook is missing, so we require BOTH the
// PermissionRequest http hook AND the statusLine shim (see claude-code-settings.snippet.json comment).
public enum HooksDetection {

    public static let beaconHookURL = "http://127.0.0.1:8765/hook"

    // `settings` is a parsed ~/.claude/settings.json. `shimPath` is the absolute install path the
    // statusLine command must reference. Returns false for any missing/wrong-shaped key.
    public static func isInstalled(settings: [String: Any], shimPath: String) -> Bool {
        guard permissionRequestHasBeaconHook(settings) else { return false }
        guard let command = (settings["statusLine"] as? [String: Any])?["command"] as? String else { return false }
        return command.contains(shimPath)
    }

    private static func permissionRequestHasBeaconHook(_ settings: [String: Any]) -> Bool {
        guard let hooks = settings["hooks"] as? [String: Any],
              let wrappers = hooks["PermissionRequest"] as? [[String: Any]] else { return false }
        return wrappers.contains { wrapper in
            guard let inner = wrapper["hooks"] as? [[String: Any]] else { return false }
            return inner.contains { ($0["url"] as? String) == beaconHookURL }
        }
    }
}
