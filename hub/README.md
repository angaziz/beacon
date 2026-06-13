# Beacon Hub (macOS)

The menubar app that feeds the Beacon device its private-plane data over Bluetooth LE:

- **AI Usage** — reads Claude Code and Codex usage/limits on the Mac and streams the normalized numbers to the device (Claude via the Claude Code statusline `rate_limits`; Codex via `~/.codex`).
- **Coding Buddy** — bridges Claude Code session state and tool-permission prompts to the device, so you can approve or deny from the desk gadget. The decision is enforced on the Mac.
- **Secrets stay here.** Tokens and credentials never leave the Mac; the BLE frames carry only normalized usage values and prompt metadata. Protocol + policies: [`CONTRACT.md`](CONTRACT.md).

<img src="../docs/assets/hub.png" alt="Beacon Hub menubar panel" width="380">

Requirements: macOS 13+, Bluetooth, [Claude Code](https://claude.com/claude-code) for the Claude features (Codex optional). `jq` is needed for the hooks installer (`brew install jq`).

## Install

### From a release (no toolchain)

1. Download `Beacon-Hub-<version>-macos-apple-silicon.zip` from [Releases](https://github.com/angaziz/beacon/releases), unzip, and drag **Beacon Hub.app** to `/Applications`. Releases are Developer ID-signed and notarized — Gatekeeper opens them without complaint. (hub-v0.1.0 was ad-hoc signed; if macOS blocks it, use **System Settings > Privacy & Security > Open Anyway**.)
2. Launch it. Beacon Hub lives in the menubar (no Dock icon).

### From source

Xcode 16+ (or the command-line tools) is the only dependency (`swift build` may work on Xcode 15, but `swift test` trips a swift-driver crash on Swift 5.10 — CI uses the Swift 6 toolchain):

```bash
cd hub
./build-app.sh run        # build, sign ad-hoc, launch with logs in this terminal
```

A bundled `.app` is required at runtime — macOS ties the Bluetooth permission to a signed app bundle, so a bare `swift run` is killed by TCC before it can even prompt. `swift build` and `swift test` still work for compile/unit checks.

## First run

The **Set up Beacon** window walks three checks:

1. **Bluetooth** — approve the system prompt (or use *Open Bluetooth Settings* if you declined it earlier).
2. **Device** — power on the Beacon device; macOS prompts to pair and the link is bonded.
3. **Claude Code hooks** — click **Install hooks**. This merges Beacon's hooks and a statusline wrapper into `~/.claude/settings.json`: a timestamped backup is written first, the merge is idempotent, and an existing statusline command keeps working (Beacon wraps it rather than replacing it). Restart Claude Code afterwards.

What the hooks actually do:

- The hook entries POST Claude Code lifecycle events (session state, tool-permission prompts) to the hub at `127.0.0.1:8765` — see [`claude-code-settings.snippet.json`](claude-code-settings.snippet.json).
- The statusline shim ([`statusline-shim/beacon-statusline`](statusline-shim/beacon-statusline), installed to `~/.beacon/`) forwards the `rate_limits` payload Claude Code already computes, which is where the usage numbers come from.

## Day to day

- The menubar panel shows the BLE connection state, Claude/Codex usage bars, and pending buddy prompts.
- Optional **launch at login** toggle.
- If the device disconnects while a prompt is pending, the hub **auto-denies it loudly** (alert on the Mac) rather than leaving it hanging — policies in [`CONTRACT.md`](CONTRACT.md) §D.1.
- Quitting drains pending prompts gracefully.

## Forget / uninstall

- **Forget Beacon** (in the menubar panel) unpairs the device; you can also remove it under System Settings > Bluetooth > info button > *Forget This Device*.
- To remove the hooks, restore the newest `~/.claude/settings.json.bak.<timestamp>` backup (or delete the Beacon entries by hand), then delete `~/.beacon/`.
- Delete the app.

## Development

```bash
swift build               # compile check
swift test                # unit tests (BeaconHubKit)
./build-app.sh release    # the bundle that CI ships on tagged releases
```

The package is plain SwiftPM (open `Package.swift` in Xcode if you prefer). `BeaconHubKit` holds the pure, host-testable logic (frame protocol, usage normalization); `beacon-hub` is the menubar agent (CoreBluetooth, pollers, hook bridge, UI).
