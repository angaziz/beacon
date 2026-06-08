# Beacon firmware — build & flash

PlatformIO project for the Waveshare ESP32-S3-Touch-AMOLED-2.16. Spike sketches (Arduino IDE) live under `docs/spikes/`; this is the product firmware (`tech.md` §12).

## Toolchain (pinned for a reason — read before changing versions)

The version matrix is tighter than `tech.md` §5 implies. Discovered empirically during P0-A.

| Component | Pin | Why this exact pin |
|---|---|---|
| Arduino-ESP32 core | **3.3.5** (pioarduino `55.03.35`) | core **3.3.6+** changed `spiFrequencyToClockDiv()` to a 2-arg signature that GFX 1.6.4/1.6.5 don't handle, so their SPI databus files fail to compile. 3.3.5 is the newest 3.3.x that still compiles GFX 1.6.4. |
| GFX_Library_for_Arduino | **1.6.4** (`tech.md` §5) | CO5300 + QSPI support; do not bump core past 3.3.5 until a GFX release adds the 2-arg signature. |
| LVGL | **8.4.0** | `lv_conf.h` (in `src/`) is required from the first build — PlatformIO compiles LVGL even if no source `#include`s it yet. |
| Python (PlatformIO runtime) | **3.13** | pioarduino `55.03.35` builder requires Python 3.10–3.13. Homebrew's `platformio` formula runs on 3.14 (rejected), and Xcode's bundled 3.9 is rejected by PlatformIO itself. |

## One-time setup (macOS, Apple Silicon)

```bash
brew install python@3.13
/opt/homebrew/opt/python@3.13/bin/python3.13 -m venv ~/.beacon-pio
~/.beacon-pio/bin/pip install -U pip platformio pyyaml   # pyyaml: pioarduino builder imports it
```

Use the venv's `pio` for everything (a bare `pio` from Homebrew runs on Python 3.14 and will fail this project). Optional convenience: `alias bpio="$HOME/.beacon-pio/bin/pio"`.

## Build / flash / monitor

```bash
cd firmware
~/.beacon-pio/bin/pio run                 # build
~/.beacon-pio/bin/pio run -t erase        # one-time: clears prior Arduino-IDE 4MB bootloader state
~/.beacon-pio/bin/pio run -t upload       # flash
~/.beacon-pio/bin/pio device monitor      # serial @115200 (ctrl-] to exit)
```

Board settings (PSRAM OPI, 16MB flash, partitions, USB-CDC) are pinned in `platformio.ini`, so they are immune to the Arduino-IDE "Tools menu silently reverts" gotcha in `docs/spikes/SETUP.md`.
