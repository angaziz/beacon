# Hardware Spikes

Throwaway experiments run on the real **Waveshare ESP32-S3-Touch-AMOLED-2.16** to de-risk the build *before* writing the firmware. They live here for transparency — they are **not** the product firmware (that will live under a top-level `firmware/` once the build phase starts). Organized by topic; each is a self-contained Arduino sketch.

## Topics

| Topic | Question | Result |
|---|---|---|
| [`display-power/`](display-power/) | Power the AXP2101 rails + bring up the CO5300 display? | **PASS** — display lit; found the AXP rail-init order the stock demo omits |
| [`wifi-ble-coexistence/`](wifi-ble-coexistence/) | Do WiFi + BLE + TLS coexist with memory to spare? | **PASS** — ~160 KB min heap, 8.3 MB PSRAM free, live TLS fetch, 0 crashes |

Each topic folder has its own `README.md` (question, result, findings) and the sketch.

## Running them

Shared toolchain + library setup, plus flashing/troubleshooting tips, are in **[SETUP.md](SETUP.md)**. Flash `display-power/beacon_power_test` first (lights the panel, confirms your board), then `wifi-ble-coexistence/beacon_coex_spike` (edit `WIFI_SSID`/`WIFI_PASS` first, 2.4 GHz).

## Where the learnings went

- [`../../DESIGN.md`](../../DESIGN.md) — "Technical constraints & risks" (coexistence proven, AXP init, 466×466 panel, rounded-display safe area).
- [`../research/`](../research/) — device + integrations research.
