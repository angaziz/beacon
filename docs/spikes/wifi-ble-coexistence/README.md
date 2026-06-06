# Spike: WiFi + BLE Coexistence

**Question:** Do WiFi + BLE + TLS coexist on the ESP32-S3 with memory to spare?
**Result: PASS** — the BLE-hub + WiFi-direct architecture is green-lit; the LAN-WebSocket fallback is **not** needed.

Sketch: [`beacon_coex_spike/`](beacon_coex_spike/) (edit `WIFI_SSID` / `WIFI_PASS` first) · Toolchain setup: [`../SETUP.md`](../SETUP.md)

## Result (all subsystems live together)

| Metric | Value |
|---|---|
| Min free internal heap | **~160 KB** |
| Free PSRAM | 8.38 MB (available for LVGL buffers) |
| WiFi STA | connected |
| TLS fetch (Open-Meteo, keyless) | HTTP 200, ~1.5 s, parsed live temp |
| BLE | advertising, no crash |
| Crashes | 0 |

That ~160 KB minimum is with the heavier **Bluedroid** stack; NimBLE would leave more.

## Findings

- **BLE stack:** NimBLE-Arduino **1.4.x crashes** on arduino-esp32 3.3.9 (IDF5) — a host/controller version mismatch (`MAGIC fadebead VERSION`). **Bluedroid (built into the core) works** and is version-matched. Use Bluedroid for now; revisit NimBLE 2.x only if internal RAM ever gets tight (it won't — LVGL buffers live in PSRAM).
- **TLS:** the spike used `setInsecure()` to isolate the radio/memory question. Production validates certs (bundle roots) per [`../../DESIGN.md`](../../DESIGN.md).
- **Not yet tested:** BLE under an **active connection** (advertising + stack init is the bulk of the cost; a live link adds a little). Worth a quick check with nRF Connect.
