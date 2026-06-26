# Spike — device audio (ES8311) pin map + feasibility (issue #110, Phase 3)

**Date:** 2026-06-26  **Status:** pins confirmed; heap-under-load measurement pending the build.

## Authoritative pin map (Waveshare ESP32-S3-Touch-AMOLED-2.16 official docs)
Source: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16 (GPIO Pin Assignments + Peripheral Quick Reference)

| Signal | GPIO | Notes |
|---|---|---|
| `I2S_MCLK` | 42 | shared ES8311/ES7210 master clock |
| `I2S_SCLK` / BCLK | 9 | shared bit clock |
| `I2S_LRCK` / WS | 45 | L/R word-select |
| `I2S_DSDIN` | 8 | ESP32 → ES8311 audio data (speaker playback path) = ESP I2S **DOUT** |
| `PA_CTRL` | 46 | speaker amplifier enable (drive high to unmute) |
| I2C SCL/SDA | 14 / 15 | shared bus (AXP2101, touch, IMU, RTC, codec) — already initialized by `hal/` |
| ES8311 I2C addr | 0x18 | ES8311 default 7-bit address (datasheet) |

`I2S_ASDOUT`=GPIO10 is the **ES7210 mic** ADC output (capture path) — NOT needed for playback; leave unused.

## Collision check (vs `firmware/src/config/pins.h`)
Display QSPI = GPIO 4/5/6/7/38/12/39; touch INT/RST = 11/40; USB = 19/20; RTC/IMU INT = 13/17/21.
**Audio pins 8/9/42/45/46 collide with NONE of these.** I2C 14/15 is the existing shared bus (additive use, distinct address). Safe to bring up audio without disturbing display/touch.

## Risk assessment
- Wrong ES8311 register init → silent (no sound), NOT dangerous; iterate.
- Pins confirmed → no display/touch break risk.
- Heap is the real gate (tech.md ≥60 KB internal floor; transient min ~53 KB today). I2S DMA buffers (internal SRAM) add ~2-4 KB. MUST measure min-free internal heap under active BLE + cert TLS + LVGL before committing device audio.

## Plan
Build the audio HAL (I2S std mode + ES8311 init over the existing I2C, PA enable on GPIO46), play a short chime on waiting/attention transitions behind a build flag, and instrument `esp_get_free_internal_heap_size()` min-free each second under load. GO only if it stays ≥60 KB with margin and TLS fetches still succeed.

## RESULT — NO-GO (measured 2026-06-26)
Flashed the `audiospike` env (full product firmware + I2S/ES8311 + a chime every 15 s + per-second heap log) and captured 123 `HEAPSPIKE` samples over ~2 min under live WiFi+BLE+TLS+LVGL load:

| Metric | Value |
|---|---|
| free internal at boot | 184,920 B |
| **lowest free internal under load (with audio)** | **49,468 B (~48.3 KB)** |
| 60 KB NFR floor | **BREACHED** (−10.5 KB) |
| no-audio transient min (docs/tech.md baseline) | ~53 KB |
| audio path cost | ~5 KB (I2S driver + DMA + ES8311 + chime buffer) |

**Decision: device audio does NOT ship.** It pushes the transient internal-heap min from ~53 KB to ~48 KB, below the ≥60 KB floor that protects TLS handshakes. Hub-only audio (`beacon-prompt.wav` + `beacon-attention.wav`) remains the chime mechanism. The spike code (`hal/audio.*`, `[env:audiospike]`, the `BEACON_AUDIO_SPIKE`-gated instrumentation in `main.cpp`/`pins.h`) is left in the tree **excluded from the product `beacon` build** as a documented artifact for any future revisit (e.g. shrinking the LVGL PSRAM/internal split, or audio-only-when-idle). The normal `beacon` build is byte-for-byte unchanged.
