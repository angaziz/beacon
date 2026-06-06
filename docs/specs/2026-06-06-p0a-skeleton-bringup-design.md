# P0-A — Skeleton + Bring-up (design)

> First of four P0 chunks. Delivers a flashable `firmware/` tree that boots the proven
> power/display init, brings up touch, runs LVGL with the mandated partial-render strategy,
> renders a test screen inside the safe area, and **freezes** the two cross-phase artifacts
> every later phase depends on: `SAFE_INSET` and `partitions.csv`.
>
> Authority: `prd.md` (what), `tech.md` (how — wins on conflict), `DESIGN.md` (token/safe-area values).
> Covers FRs: FR-PLAT-1 (boot/power/display), FR-PLAT-4 (safe area), FR-PLAT-9 (lock SAFE_INSET +
> freeze partitions), and the foundation of FR-STATE-3 (UI never hangs). Touch read/init is the
> main unproven-hardware item (`tech.md` §13).

## 1. P0 context (where this fits)

P0 is split into four independently buildable, human-testable chunks. This spec is **Chunk A**.

| Chunk | Delivers | Freezes |
|---|---|---|
| **A (this)** | PlatformIO project, `partitions.csv`, config, HAL (power/display/touch), LVGL port, logger | `SAFE_INSET`, `partitions.csv` |
| B | `DataStore`/`screen_state_t`/`HubLink` + config schemas; theme engine + 7-theme catalog; Editorial fonts + asset budget | all FR-STATE-0 contracts |
| C | Swipe carousel + 6 safe-area shells + live theme switch | — |
| D | NVS + WiFi provisioning + Settings + time service + sleep | — |

Order: A => B => (C, D overlap). B's frozen contracts unblock P1/P2/P4 parallelization (`prd.md` §7).

## 2. Goal & non-goals

**Goal.** On the Waveshare ESP32-S3-Touch-AMOLED-2.16 in hand: a PlatformIO firmware that

1. boots through the AXP2101 rail init, then CO5300 display (no black screen after a PWR-button cycle),
2. reads the CST92xx touch panel correctly (the unproven part),
3. runs LVGL 8.4 with two partial draw buffers, choosing the buffer memory region by the
   `tech.md` §8 heap floor and asserting + logging the choice,
4. renders a test screen entirely inside `SAFE_INSET` that responds to touch,
5. ships the two frozen artifacts: `SAFE_INSET` locked against the real panel, and an
   OTA-reserving `partitions.csv`.

**Non-goals (deferred to later chunks):** RTC/time service, IMU, WiFi/networking, NVS persistence,
theme tokens/catalog, the carousel, real screens, the `DataStore`/`HubLink` contracts. Chunk A
needs none of these and must not pull them in.

## 3. Architecture

```
firmware/beacon/
├── platformio.ini          # pioarduino platform (Arduino core 3.3.x), OPI PSRAM, 16MB, pinned libs
├── partitions.csv          # FROZEN — OTA layout (§6)
├── src/
│   ├── main.cpp            # bring-up sequence (tech §3) + LVGL test screen wiring ONLY
│   ├── lv_conf.h           # LVGL 8.4 config (16-bit color, mem, log)
│   ├── config/
│   │   ├── pins.h          # verified pins (from the spikes)
│   │   └── layout.h        # SCREEN_W/H, SAFE_INSET, CORNER_R (§5)
│   ├── hal/
│   │   ├── power.{h,cpp}   # AXP2101 rail init (ported from beacon_power_test)
│   │   ├── display.{h,cpp} # CO5300 init + brightness (DCS 0x51)
│   │   └── touch.{h,cpp}   # CST92xx reader behind a minimal read iface (§4.3)
│   ├── ui/
│   │   └── lvgl_port.{h,cpp} # buffers + flush cb + indev read cb + tick; heap-floor decision (§4.4)
│   └── util/
│       └── log.h           # [BEACON] level-gated serial logging
```

Each module has one responsibility and a small surface. HAL modules expose plain C-style init/read
functions; `lvgl_port` depends on `display` (flush target) and `touch` (indev source) only.

### 3.1 Module contracts (Chunk A surface)

```c
// hal/power.h
bool power_begin();                       // AXP2101 rails; false on I2C fail

// hal/display.h
bool display_begin();                     // CO5300 init + panel cmds; gfx ready
void display_brightness(uint8_t level);   // DCS 0x51

// hal/touch.h
bool touch_begin();                       // CST92xx init over I2C@0x5A, INT pin
bool touch_read(int16_t* x, int16_t* y);  // true if a point is currently down

// ui/lvgl_port.h
bool lvgl_port_begin();                   // buffers + driver reg; asserts heap floor, logs region
void lvgl_port_tick();                    // pumped from the Core-1 loop/task
```

These are Chunk-A-local helpers, **not** the frozen `HubLink`/`DataStore` contracts (those are Chunk B).

## 4. Key technical decisions

### 4.1 Build platform
PlatformIO with the **`pioarduino`** community platform fork — the only way to get Arduino-ESP32
core **3.3.x** in PlatformIO (the official `platformio/espressif32` lags at core 2.x, which fails
`tech.md` §5's pinned versions). `platformio.ini` essentials:

- `platform = https://github.com/pioarduino/platform-espressif32/releases/download/<TAG>/platform-espressif32.zip`
  where `<TAG>` is the pioarduino release that bundles **Arduino-ESP32 core 3.3.x**. Pin the exact
  tag during implementation and confirm it at boot via the `ESP.getCoreVersion()` log line (§9) —
  the build fails the `tech.md` §5 version gate if it resolves to anything other than 3.3.x.
- `board = esp32-s3-devkitc-1` as a base
- `board_build.arduino.memory_type = qio_opi`  (OPI PSRAM)
- `board_upload.flash_size = 16MB`, `board_build.flash_mode = qio`, `board_build.f_flash = 80000000L`
- `board_build.partitions = partitions.csv`, `board_build.filesystem = littlefs`
- `build_flags = -DBOARD_HAS_PSRAM -DLV_CONF_INCLUDE_SIMPLE -I src -DARDUINO_USB_CDC_ON_BOOT=1 -DCORE_DEBUG_LEVEL=3`
  (LVGL finds `src/lv_conf.h` via `-I src`; do **not** use `-DLV_CONF_PATH` — LVGL 8.4 double-stringifies it and the include breaks)
- `lib_deps` pinned to `tech.md` §5: LVGL 8.4.0, GFX_Library_for_Arduino 1.6.4, XPowersLib 0.2.6,
  SensorLib 0.3.3 (present for B/D; not exercised in A)
- `monitor_speed = 115200`

### 4.2 Power + display (low risk — port the proven spike)
Port the exact init from `docs/spikes/display-power/beacon_power_test`: `Wire.begin(15,14)` =>
AXP2101 rails (`setDC1Voltage(3300)`, ALDO1/2/4 = 3300, enable ALDO1-4; **ALDO2 = DSI_PWR_EN** is
the critical one) => `Arduino_ESP32QSPI` + `Arduino_CO5300(bus, RST2, 0, 466, 466, 0,0,0,0)` =>
`gfx->begin()` => panel cmds `0x36/0xA0`, `0x53/0x20`, `0x51/0xFF`. Brightness via raw DCS `0x51`
(no `Display_Brightness` in GFX 1.6.4).

### 4.3 Touch (the real risk — bring up first)
SensorLib covers the IMU/RTC but **not** the CST9220/CST9217. Port Waveshare's official CST92xx
reader (repo `waveshareteam/ESP32-S3-Touch-AMOLED-2.16`) behind the minimal `touch_read()` iface
above: I2C @ `0x5A`, INT on pin 11. Verify by logging tap coordinates and confirming they map to
the 466x466 panel with correct orientation (matching the display's MADCTL `0xA0`).

**Decision point inside A:** the Waveshare exact-panel driver is the primary source. SensorLib is
**not** confirmed to cover CST9220/CST9217 (`tech.md` §5 lists it only for QMI8658/PCF85063) — only
consider it as a fallback *after* verifying it actually supports this controller. Resolve before
wiring the LVGL indev.

### 4.4 LVGL port (memory is the constraint)
Per `tech.md` §6: LVGL **partial render**, **two draw buffers each ~1/10 screen**
(466 x 47 x 2 B = ~44 KB). **No full-screen framebuffer.** At boot, `lvgl_port_begin()`:

1. attempts allocation in **internal DMA-capable SRAM** (`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`),
2. checks the `tech.md` §8 floor would still hold (>= 60 KB free internal heap); if not, frees and
   reallocates the buffers in **PSRAM**,
3. **logs the chosen region** and the post-init free-internal-heap minimum, and **asserts** the floor.

This proves the floor for the **Chunk-A idle baseline only** (no WiFi/BLE/TLS yet). `tech.md` §2/§8
require the floor to be **re-measured under an active bonded BLE link + cert-validated TLS + the full
LVGL UI at P2** before the architecture is declared closed — A does not, and cannot, prove that.

Flush callback pushes the dirty rect to `gfx`; the LVGL `indev` read callback calls `touch_read()`.

### 4.5 Core/task model (partial, per tech §6)
LVGL tick + `lv_timer_handler` run on **Core 1**. The Core-0 data-task scaffolding is **not** needed
in A (no networking) and lands with the `DataStore`/net layers (B/D). The **watchdog is enabled** and
the render loop never blocks — establishing the FR-STATE-3 "never hang" foundation.

## 5. Freeze artifact 1 — `SAFE_INSET` (config/layout.h)

```c
#define SCREEN_W   466
#define SCREEN_H   466
#define CORNER_R   90      // ~20% of 466 (DESIGN.md); confirm on hardware
#define SAFE_INSET 40      // every screen lays out inside this inset rounded-rect
```

**Lock procedure (on hardware):** draw the cyan border + a rect inset by `SAFE_INSET` and verify on
the panel that (a) the outer border is fully visible (confirms the 466 working size, not 480) and
(b) no inset content is clipped by the corner arcs. Adjust `CORNER_R`/`SAFE_INSET` only if the panel
disagrees, then **lock** the values. Per `DESIGN.md`: at 40 px the content rectangle's corners sit
inside an arc up to ~96 px radius, so 40 is the floor — do not reduce.

## 6. Freeze artifact 2 — `partitions.csv` (OTA-reserving, 16MB)

Default per `tech.md` §11 open-question: **reserve OTA from the start** (changing the layout later
reflashes everything). Two ~3 MB app slots + nvs + a large littlefs data partition for fonts/assets
(Chunk B sizes the font/asset manifest against this) + a small coredump for observability.

```
# Name,    Type, SubType,  Offset,   Size
nvs,       data, nvs,      0x9000,   0x5000
otadata,   data, ota,      0xe000,   0x2000
app0,      app,  ota_0,    0x10000,  0x300000
app1,      app,  ota_1,    0x310000, 0x300000
spiffs,    data, spiffs,   0x610000, 0x9E0000
coredump,  data, coredump, 0xff0000, 0x10000
```

(`spiffs` subtype label hosts a **littlefs** filesystem via `board_build.filesystem = littlefs`.)
The bootloader + partition table occupy `0x0`-`0x9000` (not a partition); the table above is
contiguous from `0x9000` and ends exactly at `0x1000000`, so the whole layout fits 16 MB with no
gap or overrun. The OTA *update UI* (FR-SET-6) is out of scope; only the layout is frozen here.

## 7. Build sequence (each step independently flashable)

1. **Toolchain in-tree** — PIO project compiles + flashes a no-op; serial shows `[BEACON] boot`.
2. **Power + display** — port the proven init; cyan-border test => **lock `SAFE_INSET`** (§5).
3. **Touch** — bring up CST92xx; tap logs correct, correctly-oriented coords.
4. **LVGL port** — partial buffers + heap-floor decision asserted + region logged (§4.4).
5. **Test screen** — an LVGL screen inside `SAFE_INSET` (e.g. a label + a touchable button echoing
   tap coords) that visibly reacts to touch.
6. **Partitions** — flash with `partitions.csv`; confirm both OTA slots present and boot succeeds;
   measure boot time to first `lv_scr_load`.

## 8. Acceptance (hardware-verified)

- Boots through AXP2101 => CO5300 with **no black screen after a PWR-button power cycle**.
- Cyan-border test passes; `SAFE_INSET`/`CORNER_R` locked; no inset content clipped.
- Touch reports correct coordinates and orientation across the panel.
- LVGL renders via partial buffers; serial logs the buffer region (internal vs PSRAM) and the
  **Chunk-A baseline min free internal heap >= 60 KB** (`tech.md` §8). This is the idle baseline;
  the production NFR gate re-measures the floor under active BLE + cert TLS + LVGL at **P2**.
- Test screen reacts to touch with **< 100 ms** touch-to-visual feedback.
- **Boot < 4 s** to first rendered screen (`tech.md` §8; WiFi/NTP not in A, so not counted).
- `partitions.csv` flashes; both OTA slots present; firmware boots from `ota_0`.
- Builds clean against the pinned `tech.md` §5 versions; no secrets in source; `[BEACON]` logs only.

## 9. Risks & mitigations

| Risk | Mitigation |
|---|---|
| **CST92xx touch driver** (main unproven item) | Port Waveshare's exact-panel driver first; SensorLib CST fallback; bring up before LVGL indev (step 3). |
| `pioarduino` platform / core 3.3.x mapping drift | Pin a specific `pioarduino` release tag; confirm `ESP.getCoreVersion()`/IDF at boot in logs. |
| Buffer region: internal SRAM floor not held | Runtime fallback to PSRAM already specified (§4.4); region asserted + logged, never silent. |
| Board-settings drift on port re-enumeration (spike gotcha) | PlatformIO pins all of these in `platformio.ini` — immune to the Arduino-IDE Tools-menu reset described in `SETUP.md`. |
| Corner radius differs from ~90 px assumption | Cyan-border test measures it on hardware before locking (§5). |

## 10. Testing approach

Chunk A is hardware/IO-bound, so verification is the on-device acceptance run in §8 (serial-log
evidence + visual confirmation). No host unit tests here — pure-logic units (contracts, config
parsing, state machine) arrive in Chunk B, which introduces the PlatformIO `native` test env.
