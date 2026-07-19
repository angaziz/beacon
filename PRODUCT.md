# Product

## Register

product

## Users

A single owner-user: a developer at their desk. Context is *peripheral* — they glance at a 2.16" AMOLED device next to the keyboard while doing other work, occasionally reach out to act on it (approve a prompt, skip a track), rarely sit and "use" it. The job: stay aware of things that matter (AI usage limits, markets, time/weather) and take small actions without breaking focus on the Mac.

## Product Purpose

Beacon is a personal desk command-center on an ESP32-S3 AMOLED device. It is:
- A **companion to the Mac** for AI-coding: shows usage and session state from pluggable agent providers (Claude Code / Codex today) over BLE, and lets the user approve/deny tool-permission prompts and launch tasks from the device. Secret tokens stay on the Mac hub; the device never holds Claude/Codex credentials.
- **Independent over WiFi** for public, always-useful data: live FX/crypto/indices, weather, time, and Spotify control (commands an active Spotify Connect device — the device is a remote, not a player). These keep working when the Mac is asleep, though Spotify still needs some active player to target.

Success = the user trusts a glance at Beacon instead of checking menubars/phone, and acts on it (approve, skip, check a rate) without context-switching away from their work.

## Brand Personality

Dark, futuristic, efficient. A calm precision instrument, not a toy and not a billboard. Voice is terse and technical (labels, not sentences). It should feel like equipment — something an engineer keeps on the bench — rendered with restraint and craft. Gesture- and (later) voice-forward.

## Anti-references

- Generic stock Material / iOS default UI (no distinct character).
- Glossy / skeuomorphic realism (bevels, faux-metal, drop-shadow gloss).
- Cluttered widget dashboards (too many tiles, no breathing room).
- Cute consumer smartwatch (cartoony, rounded, playful).

## Design Principles

1. **Glanceable first.** The primary value reads from ~0.5-1m without leaning in. Hierarchy is brutal: one or two things are big, everything else recedes.
2. **Black is the canvas.** Pure black backgrounds — on AMOLED those pixels are physically off, so black is both the aesthetic and the power strategy. Color is spent sparingly.
3. **One instrument, many faces.** The UI is themeable. Default is Editorial; the same data renders through any theme. Each theme is a **bespoke experience** — its own per-screen layout in a distinct visual language (rings, an analog face, dot-matrix, a scope trace…), composed from shared tokens + components, not just a recolor. One theme is resident at a time; switching rebuilds the active screen.
4. **Earned motion.** Motion is purposeful and subtle (state changes, reveals), never decorative. Reduced-motion always has a calm alternative.
5. **Honest data.** Show real state and real reset windows; no invented precision, no fake gauges. If a value is stale or unknown, say so.
6. **Efficient density.** Information-forward without clutter. Add data only when it earns the space; prefer typography and hairlines over boxes and cards.

## Accessibility & Inclusion

- Body/data text holds >=4.5:1 contrast on black; large display figures >=3:1.
- Legible at arm's length (~0.5-1m): generous figure sizes, tabular numerals.
- Reduced-motion alternative for every animation (crossfade/instant).
- Never rely on color alone for state: finance up/down and alert states pair color with sign, glyph, and position.
