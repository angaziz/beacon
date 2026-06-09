#!/usr/bin/env bash
# Package beacon-hub as a signed .app bundle. macOS TCC keys Bluetooth permission to a code-signed
# app bundle (Info.plist + signature) -- a bare `swift run` binary is aborted as a privacy violation,
# even with an embedded __info_plist. Build the bundle, then run it so CoreBluetooth can PROMPT.
#
# IMPORTANT: launch via `open` (LaunchServices), NOT by running the binary path directly. macOS TCC
# only reads the bundle's Info.plist (NSBluetoothAlwaysUsageDescription) for LaunchServices-launched
# apps; a direct `./Beacon Hub.app/Contents/MacOS/beacon-hub` is aborted as a privacy violation.
#
# Usage:  ./build-app.sh [debug|release]        # build + sign the .app
#         ./build-app.sh run [debug|release]    # build + sign + launch (logs stream to this terminal)
#         ./build-app.sh install-hooks          # merge beacon hooks + statusLine into ~/.claude/settings.json (no Swift toolchain)
set -euo pipefail
cd "$(dirname "$0")"

# install-hooks is intercepted here, BEFORE any `swift build`, so it works on a fresh checkout
# with no toolchain. Inlined (this file has no functions) to avoid a define-before-use ordering bug.
if [ "${1:-}" = "install-hooks" ]; then
  # Step 0 -- dep check. jq is the only hard requirement; everything else is coreutils.
  command -v jq >/dev/null 2>&1 || { echo "error: jq not found. Install it: brew install jq" >&2; exit 1; }

  # Step 1 -- shim path. $PWD is hub/ (we cd'd to dirname $0), so this resolves correctly anywhere.
  SHIM="$PWD/statusline-shim/beacon-statusline"
  [ -f "$SHIM" ] || { echo "error: shim not found at $SHIM" >&2; exit 1; }
  [ -x "$SHIM" ] || { echo "error: shim not executable: $SHIM (chmod +x it)" >&2; exit 1; }
  case "$SHIM" in
    *" "*) echo "warning: shim path contains spaces; the space-joined statusLine string may not parse at the Claude Code layer." >&2 ;;
  esac

  SNIPPET="claude-code-settings.snippet.json"
  [ -f "$SNIPPET" ] || { echo "error: snippet not found: $PWD/$SNIPPET" >&2; exit 1; }

  # Step 2 -- locate/bootstrap settings.
  SETTINGS="$HOME/.claude/settings.json"
  mkdir -p "$HOME/.claude"
  if [ -f "$SETTINGS" ]; then
    MODE=$(stat -f '%Lp' "$SETTINGS")   # preserve the user's mode; don't broaden a private 0600 file
  else
    printf '{}\n' > "$SETTINGS"
    echo "bootstrapped empty $SETTINGS"
    MODE=644                            # CC's conventional default for a freshly created settings file
  fi

  # Step 3 -- validate existing settings is strict JSON (no JSONC). Abort before touching anything.
  jq -e . "$SETTINGS" >/dev/null 2>&1 || { echo "error: $SETTINGS is not valid JSON; refusing to merge. Fix or remove it first." >&2; exit 1; }

  # Step 4 -- timestamped backup.
  BACKUP="$SETTINGS.bak.$(date +%Y%m%d-%H%M%S)"
  cp "$SETTINGS" "$BACKUP" || { echo "error: backup failed ($BACKUP)" >&2; exit 1; }

  # Step 6 -- atomic write: write to a same-dir temp, validate, fix perms, then rename.
  # mktemp in ~/.claude guarantees the final mv is an atomic same-filesystem rename (no partial write).
  TMP=$(mktemp "$HOME/.claude/settings.json.XXXXXX") || { echo "error: mktemp failed" >&2; exit 1; }
  trap 'rm -f "$TMP"' EXIT

  # Step 5 -- the merge. All external values enter via --arg/--slurpfile; nothing is shell-interpolated
  # into the jq program. Idempotency key is the INNER hook object (type+url): hand-installed beacon
  # entries may lack a matcher, so we match the inner http-8765 object anywhere in an event's wrappers.
  # def's must precede the pipeline (a leading/trailing `|` around them is a jq syntax error).
  jq -n --arg shim "$SHIM" --slurpfile cur "$SETTINGS" --slurpfile snip "$SNIPPET" '
    def is_beacon_inner: (.type=="http") and (.url=="http://127.0.0.1:8765/hook");
    def wrapper_is_beacon: (.hooks // []) | any(.[]; is_beacon_inner);
    ($cur[0]) as $s
    | ($snip[0] | del(._comment)) as $snippet
    | reduce ($snippet.hooks | keys_unsorted[]) as $ev ($s;
        ($snippet.hooks[$ev]) as $beaconWrappers
        | .hooks[$ev] = ((.hooks[$ev] // []) as $existing
            | if ($existing | any(.[]; wrapper_is_beacon)) then $existing
              else $existing + $beaconWrappers end))
    | (.statusLine.command // "") as $oldcmd
    # Wrap, do not replace: an inline-shell renderer (env=val cmd, cmd && other, redirects, $(...)) would
    # break if we just space-prefixed the shim, so delegate it as a single `sh -c <quoted>` invocation
    # (@sh shell-escapes it). Merge {type,command} OVER the existing statusLine so sibling options
    # (padding, refreshInterval, ...) survive. Already-wrapped (idempotent) and empty cases pass through.
    | ( if ($oldcmd == $shim or ($oldcmd | startswith($shim + " "))) then $oldcmd
        elif ($oldcmd == "") then $shim
        else ($shim + " sh -c " + ($oldcmd | @sh)) end ) as $newcmd
    | .statusLine = ((.statusLine // {}) + {type:"command", command:$newcmd})
  ' > "$TMP" || { echo "error: jq merge failed; $SETTINGS left untouched (backup: $BACKUP)" >&2; exit 1; }

  # Post-validate the merge output before it replaces the live file.
  jq -e . "$TMP" >/dev/null 2>&1 || { echo "error: merged output is not valid JSON; $SETTINGS left untouched (backup: $BACKUP)" >&2; exit 1; }

  # mktemp creates 0600; restore the ORIGINAL settings mode (captured in Step 2) so a private
  # 0600 file is not silently broadened to 0644.
  chmod "$MODE" "$TMP"
  mv "$TMP" "$SETTINGS"
  trap - EXIT

  # Step 7 -- report.
  echo ""
  echo "Beacon hooks installed."
  echo "  settings: $SETTINGS"
  echo "  shim:     $SHIM"
  echo "  backup:   $BACKUP"
  echo "Restart Claude Code for the hooks + statusLine to take effect."
  exit 0
fi

RUN=0
if [ "${1:-}" = "run" ]; then RUN=1; shift; fi
CONFIG="${1:-debug}"

swift build -c "$CONFIG"
BIN=".build/${CONFIG}/beacon-hub"
APP="Beacon Hub.app"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp "$BIN" "$APP/Contents/MacOS/beacon-hub"
cp Info.plist "$APP/Contents/Info.plist"

# Ad-hoc signature gives TCC a stable identity to attach the Bluetooth grant to.
codesign --force --sign - "$APP"

echo ""
echo "Built: $PWD/$APP"

if [ "$RUN" = "1" ]; then
  echo "Launching via LaunchServices (logs below; Ctrl-C to quit)..."
  # -W waits; --stdout/--stderr stream the agent's logs to this terminal while LaunchServices
  # provides the bundle identity TCC needs for the Bluetooth prompt.
  exec open -W "$PWD/$APP" --stdout "$(tty)" --stderr "$(tty)"
else
  echo "Run it:  ./build-app.sh run        (launches via open, logs in this terminal)"
  echo "   or:   open \"$PWD/$APP\"         (background; view logs via: log stream --predicate 'process == \"beacon-hub\"')"
fi
