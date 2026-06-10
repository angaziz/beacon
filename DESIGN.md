# Design

Visual system for Beacon. The UI is **themeable**, and each theme is a **bespoke experience**: it renders the five screens in its own visual language (HUD concentric rings, Analog clock face + sub-dials, LED cell-meters, Oscilloscope graticule + traces, Blueprint dimension axes, Dot-Matrix figures, Editorial type-grid) — not just a recolor. Themes share a common foundation — design tokens, a small set of components (styles, the gauge component, the screen-state helpers), and per-theme background "chrome" — but each theme owns its per-screen layout. Default theme is **Editorial Index**. Firmware maps token groups to LVGL primitives (`lv_style_t`, `lv_font_t`, a `gauge_style` enum); switching theme rebuilds the active screen from that theme's view (one theme resident at a time).

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

`bar` (Editorial) · `ring` (HUD) · `cell` (LED) · `waveform` (Oscilloscope) · `measure` (Blueprint) · `bigfig` (Dot-Matrix) · `subdial` (Analog)

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
| **Dot-Matrix** | black | faint red | Doto (dot-matrix) | bigfig | sparse Nothing-esque dot figures |
| **Blueprint** | black | blueprint blue | Chakra Petch | measure | draftsman line-art, dimensions |
| **LED Matrix** | black | amber | Pixelify Sans | cell | dot panel, lit-pixel digits |
| **Oscilloscope** | black | phosphor green | JetBrains Mono | waveform | graticule + signal trace |
| **Analog Neo** | black | ice blue | Inter (light) | subdial | analog hands; usage sub-dials |

Every theme has bespoke per-screen layouts (each composed from the shared tokens + components), plus per-theme background chrome (grids, dot-matrix, graticule, blueprint marks). Custom-drawn widgets include the Analog clock face/hands + sub-dials, the Oscilloscope waveform trace, and the chrome backgrounds; the rest is composed from the gauge component + tokens.

**Canonical theme IDs** (NVS keys / filenames / `beacon_theme_t.id` in `tech.md`): `editorial` (default), `hud`, `dotmatrix`, `blueprint`, `led`, `oscilloscope`, `analog` — these map to the display names above (Editorial Index, Aerospace HUD, Dot-Matrix, Blueprint, LED Matrix, Oscilloscope, Analog Neo).

**Token authority:** this doc owns token *values* and the theme catalog; `tech.md` §6 owns the runtime contract (`beacon_theme_t` struct + `gauge_style_t` enum). Keep them in sync.

## Screens (MVP — 5)

1. **Home** — clock, date, weather (temp + humidity + condition). [WiFi]
2. **Finance** — config-driven ticker list: FX→IDR, BTC, indices/ETFs, IHSG. Value + signed change. [WiFi]
3. **AI Usage** — Claude and Codex, each showing **both** a 5-hour and a 7-day window (utilization % + reset). All four values, not one per provider. [BLE / Mac hub]
4. **Coding Buddy** — idle state (session count, tokens, context %, recent activity) and prompt state (tool-permission prompt with Approve/Deny). See Coding Buddy contract. [BLE / Mac hub]
5. **Settings** — battery %, Wi-Fi, brightness, **Theme picker**, tickers, sleep, about. (Battery shows level + charging, color-coded; low = `down` color.) [local / NVS]

Navigation (with `prd.md` phase): horizontal **swipe** = prev/next screen (carousel) — **P0**; **long-press** = screen context action and **swipe-down** = quick brightness — **P3**; **IMU** raise/flick = wake, shake = dismiss overlay / exit a subview (no carousel back-stack) — **P3**. Minimum touch target ~64px (~3mm) given arm's-length use; primary actions get the largest hit areas.

## Rounded display & safe area

The panel is a **rounded-square** (466×466), not a true rectangle — the four corners are cut by an arc. Design to the panel minus those arcs.

- **Assume corner radius ≈ 90 px (~20% of 466)** until measured on hardware (verify with the cyan-border test in the display-power spike).
- **Edge safe margin ≥ 40 px** on every side for all content. At 40 px the content rectangle's corners fall inside a corner arc up to ~96 px radius, so nothing clips.
- **Corner keep-out:** nothing critical or tappable inside the corner arcs. Edge-spanning rows (the `BEACON / SCREEN` eyebrow, finance/settings rows, the bottom meta row) keep their **end content ≥ 40 px from the side edges** so it isn't clipped at the top/bottom corners.
- **Anchor primary info centrally**, where arcs never reach (clock, big % figures). Corners hold only low-stakes labels (status, units), inset.
- A **tap target clipped by a corner is unreachable** — keep every hit area fully inside the safe zone.

The mockup device frames render the true rounded shape; [`docs/design/mockups/safe-area.html`](docs/design/mockups/safe-area.html) overlays the safe zone for verification, and the five current screens sit within it. Firmware: define `SAFE_INSET` once and lay every screen out inside the inset rounded-rect.

## Components

- **Clock** — display figures + date line (or analog face in Analog theme).
- **Gauge** — renders per `gauge-style` token (bar/ring/cell/waveform/measure/bigfig/subdial). Single component, token-switched.
- **List row** — label (body) + value (mono/display) + optional caret; hairline separator. Used by Finance, Settings.
- **Stat block** — name + big figure + detail line + gauge. Used by AI Usage.
- **Prompt** — alert label + tool + mono command hint + Deny/Approve split actions. Used by Coding Buddy.
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
| Reconnecting | Mac-menu-bar concern only; the peripheral cannot observe the link mid-reconnect, so there is no on-device reconnecting state (it sees connected or hub-offline) |

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
- **Tokens**: Claude/Codex credentials never leave the Mac hub.
- **Audit**: the hub logs approve/deny decisions with timestamp + prompt id.

## Technical constraints & risks

- **WiFi + BLE coexistence — proven for advertising + insecure TLS (spike 2026-06-06).** WiFi STA + TLS HTTPS + BLE **advertising** + display ran together with **~160 KB min free internal heap**, 8.3 MB PSRAM free, 0 crashes. **Still to verify at P2:** an *active bonded BLE link* + *cert-validated TLS* + the *full LVGL UI* (all use a bit more heap). Keep the `HubLink` transport abstract as insurance (LAN-WebSocket fallback). BLE stack = **Bluedroid** (built into arduino-esp32 3.x; NimBLE-Arduino 1.4.x crashes on this core). See `docs/spikes/wifi-ble-coexistence/` and `tech.md` §2.
- **AXP2101 init is mandatory before display.** Enable ALDO1/2/3/4 @3.3V on boot (ALDO2 = DSI_PWR_EN for the 2.16). Skipping it leaves the panel dark after any PWR-button power cycle. Brightness via raw DCS `0x51` (no `Display_Brightness` in the GFX build).
- **Panel is 466x466** in the working driver (not the advertised 480x480) — target layouts to 466.
- **Memory.** LVGL buffers per `tech.md` §6 (two partial draw buffers; internal SRAM if the ≥60 KB free-heap floor holds, else PSRAM; no full framebuffer). **Bluedroid** BLE (not NimBLE); one TLS socket at a time; one theme live at runtime.
- **Font / flash budget (not yet sized).** Each theme's display fonts (some 100px+) are glyph sets in flash. Subset large fonts to used glyphs (digits, `:`, `%`, `°`, A-Z as needed) and choose bpp deliberately. Produce a font/asset manifest + flash partition layout (incl. OTA slots) during the spike; "16MB is enough" must be proven against that budget, not assumed.
- **Time.** NTP over WiFi with PCF85063 RTC for offline persistence; timezone/DST configurable in Settings; weather location ties to the same locality. Reset-window math (5h/7d) uses device time, so correct TZ matters.
