# Beacon Spike — Step 1: Toolchain + Baseline

Goal of the whole spike: prove the **WiFi + BLE coexistence** path (the #1 architectural risk) on real hardware, plus display bring-up and memory headroom. We do it in two steps so we never debug a new toolchain and custom firmware at once.

**Step 1 (this file): get the toolchain working and flash Waveshare's own demo.** If their demo runs, your environment + board + display + PSRAM + power are all good, and Step 2 (the coexistence spike I'll provide) is built on the exact same init.

---

## Verified hardware facts (from Waveshare's shipping code)

Pin map (`Mylibrary/pin_config.h`):

| Function | Pins |
|---|---|
| Display QSPI | CS 12, SCLK 38, SDIO0-3 = 4/5/6/7, RST 2 |
| Display driver | CO5300 over `Arduino_ESP32QSPI` |
| **Panel size** | **466 x 466** in the code (NOT the advertised 480x480 — Step 1 confirms which is right) |
| I2C (touch, IMU, AXP2101) | SDA 15, SCL 14 |
| Touch | INT 11, RST 2 (shared) |
| PMU | AXP2101 (must power rails BEFORE display init) |

Init order that matters: `Wire.begin(15,14)` -> AXP2101 `power.begin(...)` -> `gfx->begin()`. Power first, or the panel stays dark.

---

## 1. Install Arduino IDE + ESP32 core

1. Install **Arduino IDE 2.x** (https://www.arduino.cc/en/software).
2. Arduino IDE -> Settings -> "Additional boards manager URLs", add:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. Tools -> Board -> Boards Manager -> search **esp32** -> install **esp32 by Espressif** (use a 3.x version; 3.3.x matches Waveshare's example pack).

## 2. Get Waveshare's code + libraries

```bash
git clone https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16
```

Copy these folders from `ESP32-S3-Touch-AMOLED-2.16/examples/Arduino-v3.3.5/libraries/` into your Arduino libraries folder (`~/Documents/Arduino/libraries/`):

- `GFX_Library_for_Arduino` (v1.6.4)
- `lvgl` (v8.4.0)
- `SensorLib`
- `XPowersLib`
- `Mylibrary` (provides `pin_config.h`)

**LVGL gotcha (most common failure):** copy `libraries/lv_conf.h` from the repo to the **root** of your Arduino libraries folder, i.e. `~/Documents/Arduino/libraries/lv_conf.h` (it must sit next to the `lvgl` folder, not inside it). If LVGL later errors about `lv_conf.h`, this is why. The authoritative steps are in Waveshare's guide: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16/Development-Environment-Setup-Arduino

## 3. Board / flash settings (Tools menu)

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| USB CDC On Boot | **Enabled** (so Serial works over USB-C) |
| CPU Frequency | 240 MHz |
| Flash Mode | QIO 80 MHz |
| Flash Size | 16 MB (128 Mb) |
| Partition Scheme | 16M Flash (3MB APP / 9.9MB FATFS) — **spikes only**; product firmware uses the OTA-reserving `partitions.csv` in `tech.md` §11 |
| PSRAM | **OPI PSRAM** |
| USB Mode | Hardware CDC and JTAG |

## 4. Flash a spike

Prefer **`display-power/beacon_power_test`** as the first flash — it initializes the AXP2101 power rails and lights the panel reliably. (The stock `05_LVGL_Widgets` example does **not** init the AXP, so after any PWR-button power-off its screen stays dark.) Then flash `wifi-ble-coexistence/beacon_coex_spike` (edit your WiFi creds first). Open Serial Monitor at **115200**.

## Flashing & troubleshooting (learned the hard way)

- **No dedicated RST button.** Buttons are PWR, BOOT, user-IO18. PWR: ~1 s press = ON, ~5–6 s hold = OFF.
- **"No serial data received" on upload** (common when the current firmware crash-loops): force ROM download mode — unplug → hold **BOOT** → plug in → keep holding ~3 s → release → reselect Tools → Port (the `usbmodem…` name may change) → Upload. Dropping Upload Speed to 115200 helps if flaky.
- **`partition … exceeds flash chip size 0x400000` at boot** = the on-chip bootloader still thinks flash is 4 MB. Set **Flash Size 16MB**, set **Erase All Flash Before Sketch Upload = Enabled** once, reflash (rewrites a 16 MB-consistent bootloader + partition table), then set Erase back to Disabled.
- **Tools settings silently revert** (Flash Size → 4MB, USB CDC → Disabled, partition → default) when the port re-enumerates during all that replugging. Re-check them before each upload until the firmware stops crash-looping.
- **Blank screen but the board enumerates** = display power rail off. Firmware must enable AXP `ALDO2` (DSI_PWR_EN) + `ALDO3` — see [`display-power/`](display-power/).

Per-topic spike notes: [`display-power/`](display-power/) · [`wifi-ble-coexistence/`](wifi-ble-coexistence/).
