import Foundation

// Pure dict -> Bool detection of the beacon Claude Code hooks, split out of the executable's
// HooksInstaller so it is unit-testable without touching real $HOME (the file read + Process stay in
// the executable). "Installed" is intentionally STRICT: the optional SessionStart/Stop/... hooks can
// be present while the round-trip-essential PermissionRequest hook is missing, so we require BOTH the
// PermissionRequest hook AND the statusLine shim (see claude-code-settings.snippet.json comment).
//
// A legacy `type: http` PermissionRequest hook reads as NOT installed: it still works while the hub is
// up, but prints a per-event error in Claude Code whenever the hub is down. Reporting it as missing is
// what surfaces the Install button that migrates it to the silent beacon-claude-hook shim.
public enum HooksDetection {

    // Legacy inner-hook shape (pre-shim installs). Kept so the installer's jq merge and this detection
    // agree on what "a beacon hook" is; matching it here would defeat the migration prompt.
    public static let beaconHookURL = "http://127.0.0.1:8765/hook"

    // `settings` is a parsed ~/.claude/settings.json. `shimPath` is the absolute install path the
    // statusLine command must reference; `hookShimPath` the one the hook command must equal.
    // Returns false for any missing/wrong-shaped key.
    public static func isInstalled(settings: [String: Any], shimPath: String, hookShimPath: String) -> Bool {
        guard permissionRequestHasBeaconHook(settings, hookShimPath: hookShimPath) else { return false }
        guard let command = (settings["statusLine"] as? [String: Any])?["command"] as? String else { return false }
        return command.contains(shimPath)
    }

    private static func permissionRequestHasBeaconHook(_ settings: [String: Any], hookShimPath: String) -> Bool {
        guard let hooks = settings["hooks"] as? [String: Any],
              let wrappers = hooks["PermissionRequest"] as? [[String: Any]] else { return false }
        return wrappers.contains { wrapper in
            guard let inner = wrapper["hooks"] as? [[String: Any]] else { return false }
            // Exact match: a user command hook that merely ends in "beacon-claude-hook" is not ours.
            return inner.contains { ($0["command"] as? String) == hookShimPath }
        }
    }
}
