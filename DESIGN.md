# Design

Visual system for Beacon. The UI is **themeable**: every screen is built from design tokens + a small set of components. A theme is a token set (plus a few widget choices). Default theme is **Editorial Index**. Firmware maps each token group to LVGL primitives (`lv_style_t`, `lv_font_t`, and a `gauge_style` enum), so adding a theme is data, not new screen code — except for the few outlier widgets noted below.

## Theme Model (tokens)

### Color roles
All themes use a pure-black canvas. Roles, not raw colors:

| Token | Role |
|---|---|
| `bg` | screen background — always `#000000` (AMOLED off-pixels) |
| `ink` | primary text / foreground |
| `ink-dim` | secondary text, labels |
| `line` | hairline rules, dividers, gauge tracks |
| `accent` | the one signal color (selected state, emphasis, the % sign) |
| `accent-2` | optional secondary accent (themes that use two) |
| `up` / `down` | finance direction (paired with sign + glyph, never color alone) |
| `alert` | permission / attention state |

### Typography roles

| Token | Role |
|---|---|
| `font-display` | hero figures (clock, big %, track title) |
| `font-body` | headings, labels, list rows |
| `font-mono` | data, codes, timestamps, eyebrows |

Hierarchy via scale + weight (>=1.25 step). Tabular numerals everywhere figures change. Max 3 families per theme.

### Gauge style
One token selects how levels/usage render, so structurally-similar themes share screen code:

`bar` (Editorial) · `ring` (HUD) · `cell` (LED) · `waveform` (Oscilloscope) · `measure` (Blueprint) · `bigfig` (Calm) · `subdial` (Analog)

### Other tokens
`radius` (frame + element), `stroke` (hairline/medium widths), `space` (4/8/12/16/24/32 rhythm), `glow` (accent glow amount; 0 for flat themes).

### Motion tokens
`dur-fast` 120ms · `dur` 220ms · `dur-slow` 400ms. Easing: ease-out-expo/quint (no bounce). Screen transition: horizontal slide+fade. Value changes: count-up / arc-sweep / cell-fill per gauge style. Every motion has a reduced-motion path (crossfade or instant).

## Default Theme — Editorial Index

| Token | Value |
|---|---|
| bg | `#000000` |
| ink | `#f4f3ef` |
| ink-dim | `#74726c` |
| line | `rgba(255,255,255,.14)` |
| accent | `#ff4a2b` (signal orange) |
| up / down | `up` = ink, `down` = accent (with +/- and triangle) |
| alert | accent |
| font-display | Space Grotesk (500) |
| font-body | Space Grotesk (400-700) |
| font-mono | JetBrains Mono (400-500) |
| gauge-style | `bar` |
| glow | 0 (flat) |

Editorial character: oversized tabular figures, hard hairline rules, strict left-grid, generous negative space, exactly one accent. Type carries hierarchy; no boxes/cards.

## Theme Catalog (overrides from default)

| Theme | Canvas | Accent(s) | Display font | Gauge | Signature |
|---|---|---|---|---|---|
| **Editorial** (default) | black | signal orange | Space Grotesk | bar | type-led, hairline rules |
| **Aerospace HUD** | black | cyan + amber | Rajdhani | ring | concentric gauges, hairline grid |
| **Calm Futurism** | black | faint red | Doto (dot-matrix) | bigfig | sparse, white-on-black |
| **Blueprint** | black | blueprint blue | Chakra Petch | measure | draftsman line-art, dimensions |
| **LED Matrix** | black | amber | Pixelify Sans | cell | dot panel, lit-pixel digits |
| **Oscilloscope** | black | phosphor green | JetBrains Mono | waveform | graticule + signal trace |
| **Analog Neo** | black | ice blue | Inter (light) | subdial | analog hands; usage sub-dials |

Outlier widgets needing bespoke code (not just tokens): Analog clock face/hands, Oscilloscope waveform trace. All others are token + gauge-style driven.

## Screens (MVP — 6)

1. **Home** — clock, date, weather (temp + humidity + condition). [WiFi]
2. **Finance** — config-driven ticker list: FX→IDR, BTC, indices/ETFs, IHSG. Value + signed change. [WiFi]
3. **AI Usage** — Claude and Codex, each showing **both** a 5-hour and a 7-day window (utilization % + reset). All four values, not one per provider. [BLE / Mac hub]
4. **Coding Buddy** — idle state (session count, tokens, context %, recent activity) and prompt state (tool-permission prompt with Approve/Deny). See Coding Buddy contract. [BLE / Mac hub]
5. **Now-Playing** — track / artist / progress / transport, target device. Controls an active Spotify Connect device (the device is a remote, not a player). [WiFi / Spotify]
6. **Settings** — Wi-Fi, brightness, **Theme picker**, tickers, sleep, about. [local / NVS]

Navigation: horizontal swipe = prev/next screen (carousel); swipe-down = quick brightness; long-press = screen context action; IMU raise/flick = wake; shake = dismiss overlay / exit a subview (the carousel itself has no back-stack). Minimum touch target ~64px (~3mm) given arm's-length use; primary actions get the largest hit areas.

## Components

- **Clock** — display figures + date line (or analog face in Analog theme).
- **Gauge** — renders per `gauge-style` token (bar/ring/cell/waveform/measure/bigfig/subdial). Single component, token-switched.
- **List row** — label (body) + value (mono/display) + optional caret; hairline separator. Used by Finance, Settings.
- **Stat block** — name + big figure + detail line + gauge. Used by AI Usage.
- **Prompt** — alert label + tool + mono command hint + Deny/Approve split actions. Used by Coding Buddy.
- **Now-playing** — art block + title/artist + progress line + transport state.
- **Eyebrow** — mono `BEACON / <SCREEN>` + right-side status (used sparingly, one per screen — not on every section).

## Firmware mapping

- Color/type tokens => a `Theme` struct of `lv_style_t` + `lv_font_t` pointers, applied at screen build / theme switch.
- `gauge-style` => an enum the Gauge component switches on.
- Fonts: subset large display glyphs to used characters; store in flash (16MB ample). One theme live at a time, so runtime RAM ~ one theme.
- Theme selection persisted in NVS; switch rebuilds the active screen with the new style set.

## Screen states

Every data screen has explicit non-happy states (Principle: honest data). Never show a stale value as if live.

| State | Behavior |
|---|---|
| Loading / first fetch | skeleton or dimmed last-known + a subtle activity cue |
| Stale | show last value + age ("3m ago"); past a per-source threshold, dim it and mark stale |
| Offline (WiFi down) | banner/eyebrow flag; ambient screens keep last-known with age |
| Error / rate-limited | terse cause ("rate-limited", "no route"), retry with backoff, no fake data |
| Mac hub disconnected (BLE) | AI Usage + Coding Buddy show "hub offline — last synced HH:MM"; buddy actions disabled |
| Reconnecting | show reconnecting state; re-enable actions only once the link is confirmed |

Per-source freshness: finance/weather minutes-scale; usage 30-60s poll; clock from RTC (always live). Each screen owns a `lastUpdated` timestamp.

## Coding Buddy contract

Capabilities (and hard limits) the buddy screen is built to — do not design beyond these:
- **Monitor**: session count (running/waiting), tokens, context %, recent activity — pushed from the Mac hub.
- **Approve / deny** a tool-permission prompt via Claude Code `PreToolUse` / `PermissionRequest` hooks. The hook is **blocking with ~30s timeout** — design for a <5s decision; on timeout the call is denied (fail-closed), surfaced as "timed out".
- **Launch** a new task via `claude -p` (a fresh session), optionally dictated later.
- **Cannot** answer Claude's `AskUserQuestion` multiple-choice prompts, persist "don't ask again", or type into a live TUI session. The UI must not imply these.

States: idle (monitoring), prompt-pending (the Approve/Deny screen, may auto-surface and wake the display), decision-sent (brief confirm), hub-disconnected (actions disabled).

## Security

The device can approve tool execution, so the control path is security-sensitive:
- **BLE**: LE Secure Connections bonding + device allowlist; characteristics encrypted. Only the bonded hub may drive the buddy.
- **Permission decisions**: each carries the prompt's `id`; the hub matches it to the originating request (reject stale/unknown ids) so a decision can't apply to the wrong call.
- **LAN WebSocket fallback**: bind local-only, require a shared token, reject off-LAN origins.
- **Tokens**: Claude/Codex credentials never leave the Mac hub. Device-plane tokens (Spotify refresh, Hermes) are scoped and, where possible, held by a proxy rather than on-device.
- **Audit**: the hub logs approve/deny decisions with timestamp + prompt id.

## Technical constraints & risks

- **WiFi + BLE coexistence (top risk).** The ESP32-S3 shares one 2.4GHz radio between WiFi and BLE (time-division). Beacon needs both live (device-direct WiFi + hub BLE). Validate stability + heap headroom early; fallback = run buddy/usage over LAN WebSocket so the device is WiFi-only. Keep the hub transport abstract so this swap is cheap.
- **Memory.** LVGL draw buffer in internal SRAM (partial render), framebuffer in PSRAM; NimBLE (not Bluedroid); one TLS socket at a time. One theme live at runtime.
- **Font / flash budget (not yet sized).** Each theme's display fonts (some 100px+) are glyph sets in flash. Subset large fonts to used glyphs (digits, `:`, `%`, `°`, A-Z as needed) and choose bpp deliberately. Produce a font/asset manifest + flash partition layout (incl. OTA slots) during the spike; "16MB is enough" must be proven against that budget, not assumed.
- **Time.** NTP over WiFi with PCF85063 RTC for offline persistence; timezone/DST configurable in Settings; weather location ties to the same locality. Reset-window math (5h/7d) uses device time, so correct TZ matters.
