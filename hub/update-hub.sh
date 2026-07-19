#!/usr/bin/env bash
# Rebuild the hub and swap the running instance for the new one.
#
# build-app.sh alone does NOT replace a running agent: `open` on an already-running bundle just
# re-activates the old process, so a rebuild appears to do nothing. This quits the old instance first,
# then relaunches.
#
# Usage:  ./update-hub.sh          # rebuild + relaunch in the background (menubar only)
#         ./update-hub.sh logs     # rebuild + relaunch and stream logs in this terminal
set -euo pipefail
cd "$(dirname "$0")"

APP="Beacon Hub.app"

# The device keeps its BLE bond across this: the bond lives in the OS pairing store, not the process.
if pgrep -x beacon-hub >/dev/null 2>&1; then
  echo "==> Quitting the running hub..."
  osascript -e 'quit app "Beacon Hub"' >/dev/null 2>&1 || true
  for _ in $(seq 1 20); do
    pgrep -x beacon-hub >/dev/null 2>&1 || break
    sleep 0.25
  done
  # Only escalate if it ignored the polite quit; SIGKILL would skip its BLE teardown.
  if pgrep -x beacon-hub >/dev/null 2>&1; then
    echo "==> Still running, sending TERM..."
    pkill -x beacon-hub || true
    sleep 1
  fi
else
  echo "==> No hub running."
fi

echo "==> Building..."
if [ "${1:-}" = "logs" ]; then
  exec ./build-app.sh run release        # builds, signs, launches with logs in this terminal
fi

./build-app.sh release
echo "==> Relaunching..."
open "$PWD/$APP"
echo ""
echo "Hub updated. Logs:  log stream --predicate 'process == \"beacon-hub\"' --level info"
