# Beacon

A dark, futuristic desk command-center on a 2.16" AMOLED touch device — built on the
**Waveshare ESP32-S3-Touch-AMOLED-2.16**. It sits next to your keyboard and, at a glance,
shows your Claude Code / Codex usage, live markets, weather, music, and a Claude coding
"buddy" you can approve tool-prompts on — without breaking focus on your Mac.

> **Status: early / prototype.** The design system and the on-device hardware bring-up
> (display + power + WiFi/BLE coexistence) are proven. The firmware MVP is in progress.
> Expect things to move. See [Roadmap](#roadmap).

<p>
  <img src="design/mockups/shots/editorial/edi-home.png" width="32%" alt="Home screen" />
  <img src="design/mockups/shots/editorial/edi-usage.png" width="32%" alt="AI usage screen" />
  <img src="design/mockups/shots/editorial/edi-buddy.png" width="32%" alt="Coding buddy screen" />
</p>

## What it does

Six screens, navigated by swipe + motion gestures:

| Screen | Shows | Source |
|---|---|---|
| Home | clock, date, weather, humidity | WiFi (direct) |
| Finance | configurable FX→IDR, crypto, indices, ETFs | WiFi (direct) |
| AI Usage | Claude + Codex, **both** 5h and 7-day windows + reset | Mac hub (BLE) |
| Coding Buddy | session state + approve/deny Claude tool-permission prompts + launch tasks | Mac hub (BLE) |
| Now-Playing | Spotify control (remote for an active Connect device) | WiFi (direct) |
| Settings | WiFi, brightness, theme picker, tickers, etc. | local (NVS) |

## Two-plane architecture

```
   Public internet (direct, WiFi+TLS)          Mac companion hub (BLE)
   - finance / weather / time                   - Claude + Codex usage
   - Spotify control                            - coding buddy (approve/deny, launch)
   - Hermes agent (device -> VPS)               - holds all secret tokens; none reach the device
                       \                        /
                        \                      /
                      [ Beacon device: ESP32-S3 + AMOLED ]
```

Private data (your Claude/Codex tokens) lives only on a small macOS hub app and reaches the
device over BLE. Public data the device fetches itself over WiFi, so the ambient screens keep
working when the Mac is asleep.

## Themes

The UI is fully themeable — **7 themes** built from shared design tokens (color / type /
gauge-style), default **Editorial Index**. See the full gallery at
[`design/mockups/directions.html`](design/mockups/directions.html) (open in a browser) or the
rendered previews under [`design/mockups/shots/`](design/mockups/shots/): Aerospace HUD, Calm
Futurism, Editorial, Blueprint, LED Matrix, Oscilloscope, Analog Neo.

## Repo layout

```
beacon/
├── PRODUCT.md              # product strategy: users, purpose, principles
├── DESIGN.md               # visual design system + theme tokens (the 7 themes)
├── docs/research/          # device + integrations research (hardware, APIs, prior art)
├── design/
│   ├── mockups/            # HTML theme mockups + rendered PNG previews (shots/)
│   └── tooling/            # Playwright screenshot helper (shoot.mjs)
└── firmware/
    └── spike/              # hardware spikes + setup guide (SETUP.md)
        ├── beacon_power_test/   # AXP2101 power + CO5300 display bring-up
        └── beacon_coex_spike/   # WiFi + BLE + HTTPS coexistence test
```

## Getting started (hardware)

You'll need the Waveshare **ESP32-S3-Touch-AMOLED-2.16** and a USB-C **data** cable.
Full toolchain + library setup is in **[`firmware/spike/SETUP.md`](firmware/spike/SETUP.md)**.
Short version: Arduino IDE 2.x + the `esp32` core (3.3.x), the Waveshare libraries +
`lv_conf.h`, board = *ESP32S3 Dev Module* with **PSRAM: OPI PSRAM** and **Flash: 16MB**.

Start by flashing `firmware/spike/beacon_power_test` — it powers the display correctly
(the AXP2101 rail init the stock demo omits) and confirms your board.

## Roadmap

- [x] Device + integrations research; hardware capability map
- [x] Design system + 7 themes (Editorial default)
- [x] Hardware spike: AXP2101 power + CO5300 display bring-up
- [x] Hardware spike: WiFi + BLE coexistence + memory headroom
- [ ] Foundation firmware: LVGL shell, screen carousel, settings, theme engine
- [ ] Read-only screens: Home, Finance
- [ ] macOS hub app (Swift) + AI Usage + Coding Buddy over BLE
- [ ] Now-Playing (Spotify)
- [ ] Gestures (IMU) polish
- [ ] Explore: Hermes agent, voice

## Security

- **Never commit WiFi credentials or API tokens.** The spike sketches use placeholder
  constants you edit locally; `.gitignore` excludes `secrets.h` / `.env` style files.
- Claude/Codex credentials are designed to stay on the macOS hub and never reach the device.

## Built on / thanks

- [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16) — board, drivers, examples
- [LVGL](https://lvgl.io) · [GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX) · [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) · [XPowersLib](https://github.com/lewisxhe/XPowersLib) · [SensorLib](https://github.com/lewisxhe/SensorLib)
- Prior art that informed the design: [Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter), [claude-desktop-buddy-esp32](https://github.com/vthinkxie/claude-desktop-buddy-esp32)

## Disclaimer

Personal, unofficial project. Not affiliated with or endorsed by Anthropic, OpenAI, Spotify,
or Waveshare. Some integrations rely on unofficial/unpublished endpoints that may change or
break. Use at your own risk.

## License

[MIT](LICENSE) © 2026 Anggie Aziz
