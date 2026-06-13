#!/bin/bash
# Wait for the ESP32-S3 to enumerate, then upload the capture build and run the screenshot sweep.
set -u
PIO="$HOME/.beacon-pio/bin/pio"
FW="/Users/angaziz/work/personal/beacon/firmware"
OUT="$FW/tools/capture/shots"
cd "$FW" || exit 1

find_port() { ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusbserial* 2>/dev/null | grep -v Bluetooth | head -1; }

echo "[autorun] waiting for device (up to 10 min)..."
PORT=""
for i in $(seq 1 600); do PORT=$(find_port); [ -n "$PORT" ] && break; sleep 1; done
[ -z "$PORT" ] && { echo "[autorun] no device appeared; aborting"; exit 1; }
echo "[autorun] device on $PORT — uploading capture build"

"$PIO" run -e capture -t upload --upload-port "$PORT" || { echo "[autorun] upload failed"; exit 1; }

echo "[autorun] waiting for port to re-enumerate after flash..."
sleep 3
for i in $(seq 1 30); do PORT=$(find_port); [ -n "$PORT" ] && break; sleep 1; done
[ -z "$PORT" ] && { echo "[autorun] port did not return after flash"; exit 1; }
sleep 4   # let the app boot, seed data, and bring up USB-CDC

python3 -m pip install --quiet pyserial pillow numpy 2>/dev/null
echo "[autorun] running sweep on $PORT"
python3 "$FW/tools/capture/grab.py" --port "$PORT" --out "$OUT" --montage
echo "[autorun] done -> $OUT"
ls -1 "$OUT" 2>/dev/null
