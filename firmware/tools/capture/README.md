# Device screenshots (no camera)

Pull glare-free PNGs of every theme x screen straight from the panel framebuffer. The `env:capture`
firmware mirrors the exact RGB565 strips LVGL flushes to the glass into a full 466x466 frame and
streams it over USB-CDC — these are the literal pixels sent to the display, not a re-render.

## One-time host setup

```bash
pip install pyserial pillow numpy
```

## Capture

```bash
cd firmware
~/.beacon-pio/bin/pio run -e capture -t upload     # flash the screenshot build
~/.beacon-pio/bin/pio device list                  # find the port (e.g. /dev/cu.usbmodem1101)

python3 tools/capture/grab.py --port /dev/cu.usbmodem1101 --out shots/ --montage
```

`grab.py` sends `C` to start the sweep, then writes one `shots/<theme>_<screen>.png` per frame
(7 themes x 5 screens = 35) plus `shots/montage.png` (a theme x screen contact sheet) with `--montage`.

Screens are seeded with deterministic dev data (`BEACON_DEV=1`), so the catalog is always populated
and repeatable. The sweep restores your original theme/screen when it finishes.

## Notes

- Re-run the sweep any time by re-running `grab.py` (no re-flash needed) — the device listens for `C`.
- One snapshot = one animation frame; the firmware lets each screen settle ~200 ms before grabbing.
- `env:capture` is for screenshots only — never ship it; production is `env:beacon`.
- Don't watch the same serial port in `pio device monitor` while running `grab.py` (port is exclusive).
