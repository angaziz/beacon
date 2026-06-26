# Spike — session host-context capture + tap-to-open focus (issue #110, Phase 2)

**Date:** 2026-06-26  **Status:** complete  **Outcome:** GO (tiered best-effort; precise for Warp)

## Question
What host context can a Claude Code hook subprocess capture, and how precisely can the hub
focus the originating terminal/editor per app?

## Method
Inspected the env a hook subprocess inherits (a hook runs as a child of the `claude` process in the
user's terminal) and enumerated focus mechanisms on this Mac.

## Findings

### Reachable host context (from the hook env)
| Var | Available | Notes |
|---|---|---|
| `TERM_PROGRAM` | ✅ always | identifies the app (`WarpTerminal`, `vscode`, `iTerm.app`, `Apple_Terminal`, `ghostty`, …) |
| `TERM_PROGRAM_VERSION`, `__CFBundleIdentifier` | ✅ | corroborating app id (`dev.warp.Warp-Stable`) |
| `WARP_FOCUS_URL` (`warp://session/...`) | ✅ (Warp only) | **precise session focus** — `open` it to raise that exact Warp tab |
| `WARP_TERMINAL_SESSION_UUID` | ✅ (Warp only) | per-session id |
| `CLAUDE_CODE_SESSION_ID` | ✅ | matches the hook payload `session_id` (correlation sanity) |
| `tty` / `$TTY` | ❌ | hook stdin/stdout are pipes → "not a tty"; no controlling tty |
| `TERM_SESSION_ID` / `ITERM_SESSION_ID` | ❌ (not present here) | would enable iTerm/Terminal tab targeting; absent in this env |
| `VSCODE_*` | ❌ (not present here) | this user runs Cursor, not VS Code; session was in Warp |

**Conclusion:** the only universally-reachable identifier is `TERM_PROGRAM` (which app). Precise
tab/session targeting is available **only when the app exports a focus handle in the env** — Warp does
(`WARP_FOCUS_URL`). tty-based targeting (iTerm/Terminal) is NOT reachable from the hook subprocess.

### Focus mechanisms (this Mac: Warp, Cursor, Ghostty, Terminal installed; no iTerm/VS Code)
| App (`TERM_PROGRAM`) | Focus command | Precision |
|---|---|---|
| `WarpTerminal` | `open "$WARP_FOCUS_URL"` | **session/tab (precise)** |
| `vscode` (Cursor reports this) / Cursor | `cursor -r <cwd>` (`-r` = reuse-window) | repo window |
| `ghostty` | `open -a Ghostty` | app-level |
| `Apple_Terminal` | `open -a Terminal` (no tty captured → no tab) | app-level |
| `iTerm.app` | `open -a iTerm` (no `TERM_SESSION_ID` captured → no tab) | app-level |
| unknown | `open -b "$__CFBundleIdentifier"` or no-op | best-effort |

`cursor` CLI present at `~/.local/bin/cursor` with `-r/--reuse-window`. `code` (VS Code) NOT installed.

## Design impact (Phase 2)
1. **Capture** must be a Claude Code **`command` hook** on `SessionStart` (the http hook can't read env).
   A small script reads `TERM_PROGRAM`, `WARP_FOCUS_URL` (if set), `__CFBundleIdentifier`, plus the
   stdin payload's `session_id`/`cwd`, and POSTs them to the hub (e.g. `127.0.0.1:8765/session`).
2. **Registry** stores per session: `host_app` (TERM_PROGRAM), `focus_url` (WARP_FOCUS_URL or nil),
   `bundle_id`, `cwd`.
3. **Focus resolver** (`open` command handler), tiered:
   - `focus_url` present → `open <focus_url>` (precise; covers Warp).
   - else `host_app` ∈ {vscode, Cursor} → `cursor -r <cwd>` (or `code -r` if that's the app).
   - else → `open -a <app>` / `open -b <bundle_id>` (app-level).
   - All run off the main thread, time-boxed, failure → `ack ok:false`; never block.
4. **No tty/TERM_SESSION_ID path** — drop the iTerm/Terminal "exact tab" tier from the plan; it's
   not reachable from a hook. App-level is the floor; Warp gets precise via its focus URL.

## Risk / caveats
- macOS Automation/Accessibility permission is NOT needed for `open <url>` / `open -a` / `cursor` (no
  AppleScript/System Events), so the silent-permission-hang risk from the original plan is largely
  avoided for these tiers. (If an AppleScript tab tier is added later for iTerm, that risk returns.)
- `WARP_FOCUS_URL` may embed a session token; treat as opaque, store hub-side only, never log.
