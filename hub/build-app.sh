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
set -euo pipefail
cd "$(dirname "$0")"

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
