# Spike: Display + Power Bring-up

**Question:** Can we power the AXP2101 rails and bring up the CO5300 AMOLED display?
**Result: PASS** — display lit; uncovered the AXP rail-init order Waveshare's stock demo omits.

Sketch: [`beacon_power_test/`](beacon_power_test/) · Toolchain setup: [`../SETUP.md`](../SETUP.md)

## Validated bring-up sequence

1. `Wire.begin(15, 14)` — I2C: SDA 15, SCL 14.
2. **AXP2101 first** (XPowersLib) — without this the panel stays dark:
   - `setDC1Voltage(3300)`, `setALDO1/2/4Voltage(3300)`, `enableALDO1/2/3/4()`.
   - **ALDO2 = DSI_PWR_EN** is the critical rail for the 2.16; ALDO3 is the display rail.
3. Display: `Arduino_ESP32QSPI(CS12, SCLK38, SDIO 4/5/6/7)` → `Arduino_CO5300(bus, RST2, 0, 466, 466, 0,0,0,0)` → `gfx->begin()`.
4. Panel commands via the data bus (this GFX build has **no** `Display_Brightness` method):
   - `writeC8D8(0x36, 0xA0)` MADCTL · `writeC8D8(0x53, 0x20)` enable brightness control · `writeC8D8(0x51, 0xFF)` brightness max.

## Findings

- **Panel = 466×466** in the working driver (not the advertised 480×480). Target layouts to 466.
- **AXP init is mandatory and must precede display init.** The stock `05_LVGL_Widgets` omits it and only works because factory firmware left the rails on — a PWR long-press then leaves the screen permanently dark. Beacon firmware must always init the AXP rails on boot.
- **PWR button:** ~1 s press = ON, ~5–6 s hold = OFF (via AXP PWRON). No dedicated RST button (buttons: PWR, BOOT, user-IO18).
- Baseline (display + power only, idle): ~338 KB free heap, 8.38 MB PSRAM free.
