# Beacon Hub — UX Audit & Improvement Plan

**Date:** 2026-06-08 · **Scope:** the macOS Hub experience end-to-end (install → pair → daily use → errors), the device-side hub screens, and the design intent in `docs/`. **Status of feature:** P2, in progress on `feat/p2-hub-ai`.

---

## BLUF

The Hub's **happy path is real and well-engineered**: a signed menu-bar app auto-discovers the device, OS-mediated bonding works, usage frames render across 7 themes, and the approve/deny round-trip is wired with a thoughtfully fail-*closed* timeout. The **honest-data state model is a genuine strength** — the device never fakes a live value.

But the Hub is today a **developer's bench tool wearing no UX**: every setup step is a terminal command or a hand-edited JSON file (one with the author's personal absolute path hard-coded), and **almost every failure mode collapses into an indistinguishable, silent, unrecoverable "Scanning…"**. The single most important problem is a **safety-UX gap**: when the device is disconnected, an incoming Claude permission prompt is **silently auto-denied after ~25s with zero feedback on either screen** — the user's tool call just fails for no visible reason. This is the same failure class that makes real remote-approval systems "unusable" (see openclaw exec-approval issues). Fixing the silent/ambiguous states is higher leverage than any new feature.

Severity legend: **P0** = breaks trust or blocks the user with no recovery · **P1** = significant friction/confusion · **P2** = polish.

> **Verification:** factual claims below were cross-checked by a Codex adversarial pass against the source. Corrections it surfaced are folded in (the "lying ack" P0 in §3.4b, the deny-message nuance in §3.4, the bridge-failure scope in §3.3, the statusline-shim wording in §3.1, and the touch-target scope in §3.5). All other claims verified against `file:line`.

---

## 1. How it actually works (end-to-end journey)

```
 INSTALL (CLI)              PAIR (OS-mediated)            DAILY USE                 PERMISSION BRIDGE
 ./build-app.sh run         app auto-scans "Beacon*"      menu shows usage %        CC PermissionRequest
   swift build              first match wins (no picker)  device shows LIMITS       -> POST 127.0.0.1:8765
   ad-hoc codesign          access encrypted char         + CLAUDE screens          -> hub holds HTTP open
   open .app (LSUIElement)  -> macOS pairing dialog        polls every 30-60s        -> BLE frame -> device
 BT prompt (1x/run)         device shows 6-digit passkey   last-sync age in menu     -> user taps APPROVE/DENY
 Keychain prompt (1x/build) user types code on Mac                                   -> device sends decision
 EDIT ~/.claude/settings    bond persists across restarts                            -> hub resolves hook + acks
   (hand-merge snippet,                                                              fail-closed deny at ~25s
    fix hard-coded path)
```

**Two control surfaces, split by design:** the Mac menu bar is **read-only status** (usage %, sync age, errors, Quit). The **approve/deny UI lives entirely on the device** — the Mac never shows an actionable prompt. Secrets (Claude Keychain, Codex `~/.codex`) never leave the Mac; only computed `pct`/`reset` + buddy text cross BLE.

---

## 2. What's good

| Perspective | Strength |
|---|---|
| **Honesty / trust** | Best-in-class state model. Failing providers send `null` → device shows `--`, never a re-asserted stale number. `last_updated` stamped at frame receipt; device catches "frames stopped while connected" as `ST_STALE`. Disconnect → `ST_HUB_OFFLINE` keeps last values + age, never blanks or fakes. This is the product's "honest data" principle actually honored in code. |
| **Security** | Strong, and mostly invisible to the user (good). LESC + bonding + allowlist + encrypted RX/TX; non-bonded central can't write. Per-prompt `id` matching rejects stale/unknown decisions. Hub never logs the command hint or tokens — only `id + decision + timestamp`. |
| **Permission safety** | Fail-*closed* on timeout (auto-deny at ~25s, deliberately under CC's ~30s hook timeout so CC doesn't fail-*open*). One-prompt-at-a-time serialization avoids stacking two 25s holds. Correct response-shape per event (`PermissionRequest` vs `PreToolUse`). |
| **Speed (happy path)** | Auto-scan + auto-reconnect (FR-HUB-3) means after first bond the user does nothing — it just reconnects. Usage tracks within ~60s. Decision round-trip targets <5s. |
| **Pairing feedback (device side)** | The device is an *active* participant: advertises `Beacon-XXXX`, shows a 6-digit passkey card ("PAIR WITH HUB" / "Enter this code on your Mac to pair"), auto-dismisses on bond. Touch targets get 24px hit-slop on most themes. |
| **Glanceability** | Usage screen renders all four windows (Claude/Codex × 5h/7d) with %, bar, and reset countdown across all 7 themes, with dim/severe state styling. |

---

## 3. What's not good (by perspective)

### 3.1 Easiness / onboarding — **P0/P1**
- **100% terminal + hand-edited JSON. No GUI for setup, pairing, secrets, or settings.** 6+ manual steps across two terminals and a JSON file. (NN/g and IoT-onboarding best practice: a visual step-by-step wizard with real-time feedback — none of which exists.)
- **`statusLine.command` hard-codes `/Users/angaziz/work/personal/beacon/...`** in `claude-code-settings.snippet.json:39`. **Broken out-of-box for anyone but the author**, and the statusline is the *primary* Claude-usage source — so a new user silently gets no Claude usage/context and no error. **P1** (breaks Claude usage/context, not the permission/safety path; **P0 if this is ever distributed beyond the author**).
- **The shim replaces, not wraps, an existing statusline.** The snippet sets only the shim path with no args (`claude-code-settings.snippet.json:39`); with no delegate command, `statusline-shim/beacon-statusline` falls back to printing a minimal model string (`beacon-statusline:20`) — so installing the snippet *downgrades* a user who already had a custom statusline. **P1.**
- Manual merge into `~/.claude/settings.json` with no automation/validation; easy to clobber existing hooks.
- `statusline-shim/beacon-statusline` is a shell script that exists, but `build-app.sh` doesn't install or wire it (no copy, no path substitution) — the user must do it by hand. (Correction from draft: it is not an unbuilt "binary.")

### 3.2 Discoverability / feedback — **P0 for unrecoverable states, P1 for transient ones**
- **Bluetooth-off, BT-permission-denied, device-not-found, connection-drop, and bonding-failure all collapse into one perpetual "Scanning…"** (`BeaconCentral.swift` `centralManagerDidUpdateState` default + `refreshLink`) with no remediation hint and no distinction. Severity splits: *Bluetooth off* and *permission denied* are **P0** (the user is stuck forever with no hint to fix it); ordinary *device-not-found* / *reconnecting* are **P1** (auto-resolves when the device appears). The user cannot tell "turn Bluetooth on" from "wake the device" from "I never granted permission." (Research: tell the user what to do to solve the error + what feedback to expect — absent.)
- **`MenubarController.Link.disconnected` is dead code** — `refreshLink` only ever sets `.connected`/`.scanning`, so a dropped link displays as "Scanning…", not "Disconnected."
- **Static pairing hint always shows**, even after a successful pair ("Pair: enter the code shown on the device" is permanently gray in the menu).
- **No icon state** — plain text "Beacon" / "Beacon…" / "Beacon!". No SF Symbol, no color. (Apple/menu-bar convention: template icon + distinct state glyphs.)
- **Last-sync age is computed once at render**, not live-ticking — can look stale even when fine.

### 3.3 Error recovery — **P0**
- **Bridge fails to bind port 8765 (already in use / second instance) → only a stderr line — and possibly not even that.** The `init`-time bind error is caught and written to stderr (`AppDelegate.swift:51`), but the async listener path (`stateUpdateHandler`, `ClaudeCodeBridge.swift:48-53`) **ignores `.failed`**, so a later listener failure can produce *zero* output. Either way the menu-bar user sees nothing. The **permission bridge + statusline-fed Claude usage go dead**; Claude usage degrades to the **429-prone `oauth/usage` poller fallback** (the poller starts independently, `AppDelegate.swift:31`), so it's degraded, not necessarily blank. **The safety-critical feature (permission bridge) is gone with no signal.**
- **No timeout, no escalation on any BLE failure** — bonding can loop forever with no "pairing failed, try again" path.
- **Claude Code not configured at all → completely silent** (no prompts ever reach the device; nothing flags it).
- Token refresh on 401 is **TODO/stubbed** for both Claude and Codex — expiry surfaces as `--` + a gray error line, recoverable only by manual re-login.

### 3.4 The silent auto-deny — **P0 (headline safety-UX bug)**
When a permission hook arrives while the device is **disconnected**, the hub still mints an id and holds the prompt, but `sendFrame` no-ops (not connected, `AppDelegate.swift:131`). The prompt is **invisible on the device**, hits the 25s cap, and **auto-denies**. The user's tool call fails with **nothing on the Mac and nothing on the device** — the only signal is a generic deny message inside the Claude Code TUI ("Denied on Beacon device", `Protocol.swift:97`), which doesn't name the cause, so the user can't tell "device offline" from a real deny. The **second concurrent prompt** (auto-deny-busy, `ClaudeCodeBridge.swift:143`) is denied *immediately* with the same generic message — user never told it was a busy-collision. This is exactly the remote-approval failure mode real systems get bug-reported for ("approval dialog disappears instantly," "blocks all remote usage").

### 3.4b The lying ack & the late-approve trap — **P0 (correctness, surfaced by Codex review)**
The hub **always sends `ack(ok:true)` to the device after a decision, unconditionally** (`AppDelegate.swift:82-83`) — even when `bridge.resolve` did nothing because the id is unknown or the prompt already timed out (`finish` no-ops on `done`/missing, `ClaudeCodeBridge.swift:166`). Combined with the device clearing its prompt the instant it *enqueues* the BLE decision (before any real app-level ack, `buddy_hud.cpp:28`), this produces a concrete trust break: **the user taps "Approve" a moment after the 25s cap already fired → CC has already proceeded as *denied* → the device shows the prompt vanish with a success ack → the user believes they approved.** The decision and the user's mental model silently diverge. There is no "this decision was too late / didn't apply" state anywhere.

### 3.5 Device-side state gaps — **P1**
- **`ST_RECONNECTING` is dead on the device too** — checked in every buddy view, has chip text "RECONNECTING", but **nothing ever sets it**. The device only knows `LIVE ↔ HUB_OFFLINE`; "connecting" is invisible.
- **No decision-sent confirmation** — after approve/deny the prompt just vanishes; feedback is log-only. No "approved ✓" beat.
- **No timeout/expiry UI on device** — no timer, no countdown. If the hub never updates, a prompt persists indefinitely.
- **`HUB OFFLINE` shows no timestamp** — design promised "hub offline — last synced HH:MM"; device shows only a flat "HUB OFFLINE" chip (age data exists but isn't used here).
- **App-level ack/err (`hub_parse_ack`) is implemented + unit-tested but never wired into the UI** — a hub `err` (stale/unknown id) produces no on-screen signal.
- **Inconsistent buddy touch targets across themes** — HUD, LED, Analog, and Blueprint omit `ext_click_area` entirely; Oscilloscope uses only 16px vs the 24px hit-slop on Editorial/Calm (`buddy_hud.cpp:106`, `buddy_led.cpp:97`, `buddy_oscilloscope.cpp:85`). On a glance-and-tap device this is a real reachability inconsistency, not just HUD (corrected scope from Codex). **P1.**

### 3.5b The Coding Buddy idle/activity model is wrong & unattributed — **P1**
The idle telemetry line (`%u RUNNING . %u WAITING . %uK TOK . CTX %u%%`) and the activity feed are built from Claude Code hooks whose semantics don't match how they're used. Root cause: **CC's `Stop` hook fires once per assistant *turn*, not at session end** (`ClaudeCodeBridge.swift:182-183`).

- **`WAITING` is always 0.** Never incremented — the `Notification` case deliberately `break`s to avoid a counter that climbs forever (`ClaudeCodeBridge.swift:184-188`). The line permanently reads `0 WAITING`.
- **`RUNNING` collapses to 0 and miscounts.** `SessionStart +1` / `Stop −1` (`:181-183`); since `Stop` fires every turn, a single live session decrements back to **0 after its first turn and stays there**. So during idle it reads 0, and it never reflects "N sessions running."
- **`TOK` / `CTX %` are last-writer-wins and unlabeled.** Both come from the per-session **statusline** hook (`:200-206`); with multiple sessions whichever refreshed its statusline most recently **overwrites** the values. The `120k / 12%` shown is one arbitrary session's context, not an aggregate — and nothing tells the user which session it belongs to. Ambiguous, and misleading with concurrent sessions.
- **"session stopped" is a mislabel.** `entryLine` maps `Stop → "HH:MM session stopped"` (`:260`), but `Stop` means "an assistant turn finished," so an **idle, still-alive** session displays "session stopped." Reads as an event that didn't happen.
- **No per-session attribution anywhere.** Activity entries (`:256-266`) and the permission `BuddyPrompt` (`:160`) carry no session identifier, so with 3 concurrent sessions the user can't tell which did what, or which one is asking the current permission. CC hook payloads already include `session_id`, `cwd`, and `transcript_path` — **available in the body, just unused** (`:136-137`).

### 3.5c No audible / cross-surface alert when a prompt is pending — **P1 (missed-opportunity)**
A pending permission prompt only **lights up the device** (auto-wake). If the user's attention is on the Mac — the explicit product scenario ("act without breaking focus on the Mac") — a silent visual cue on a 2.16" device next to the keyboard is easy to miss until it fail-closed denies at 25s (§3.4). There is **no sound and no Mac-side alert**. The board *can* drive audio (**ES8311 codec + amp + speaker**, `docs/research/2026-06-05-...md:46`, `tech.md:36`), but audio is currently "Explore-only" and would add I2S load to an already-tight BLE+TLS+LVGL heap (transient ~53 KB, under the 60 KB floor).

### 3.6 Persistence / lifecycle — **P1**
- **No login item.** The user must manually relaunch after every reboot/quit (and re-clear BT/Keychain prompts as applicable).
- **Keychain "Always Allow" doesn't persist across rebuilds** (ad-hoc signing changes the cdhash) — re-approve every rebuild. (Resolved only by a notarized Developer-ID build — deferred.)
- **On quit, held HTTP permission responses are dropped** — any in-flight CC prompt loses its responder (no graceful drain).
- **No "forget bond" / re-pair UX** exists (explicitly deferred to P2-F).

---

## 4. Recommended improvements (prioritized)

Grouped by leverage. Each maps to a concrete code site.

### Tier 0 — correctness & safety (do first, cheap, high trust impact)
1. **Fix the hard-coded statusline path.** Make the snippet path a placeholder + a one-line installer (`./build-app.sh install-hooks` that writes the absolute path and merges the snippet via `jq`, non-destructively). Removes the silent-no-Claude-usage trap for every non-author user. *(`claude-code-settings.snippet.json:41`, `build-app.sh`)*
2. **Never silently auto-deny invisibly.** When a permission prompt cannot be shown (device disconnected) or is busy:
   - **Return a cause-naming CC deny `message`** ("denied: Beacon device offline" / "denied: another prompt is pending") instead of the generic "Denied on Beacon device". Codex confirmed this is fully within the CC `PermissionRequest` contract (CONTRACT.md §C.3 permits a deny `message` and requires a 2xx body before the hook timeout) — so it's the cheapest reliable fix and lands right in the user's TUI. *(`ClaudeCodeBridge.swift:143,154`, `Protocol.swift:97`)*
   - Add a **persistent menu-bar alert** as the primary loud surface (a macOS notification can be turned off, so don't rely on it as the *only* signal). A user notification is a nice-to-have on top. *(`AppDelegate.swift:131`)*
3. **Make the bridge-bind failure loud.** If the listener can't bind/start 8765, show a red menu-bar state ("⚠ Bridge offline — port 8765 in use"). Fix two gaps: catch the `init` error path *and* handle `NWListener` `.failed` in `stateUpdateHandler` (currently only `.ready` is handled, so async failures are silent). *(`AppDelegate.swift:51`, `ClaudeCodeBridge.swift:48`)*
4. **Fix the lying ack first, then wire device-side confirm.** This is two steps, in order: (a) make `resolve`/`finish` report whether the decision actually applied, and have the hub send `ack(ok:false)`/`err` when it didn't (unknown/late id) instead of unconditional `ok:true` (`AppDelegate.swift:82`); (b) have the device stop clearing on local enqueue and instead show **pending → sent ✓ / "too late, re-decide"** driven by the *real* app-level ack via `hub_parse_ack` (already implemented + tested, just unwired). Without (a), wiring (b) would just confirm a lie. *(`AppDelegate.swift:82`, `ClaudeCodeBridge.swift:165`, `hub_task.cpp`, buddy views)*

### Tier 1 — make failure states legible (the biggest daily-trust win)
5. **Distinguish connection states in the menu bar.** Replace the single "Scanning…" with: *Bluetooth off* (+ "Open Bluetooth settings"), *Permission needed* (+ deep link), *Searching for device…*, *Connecting…*, *Connected*, *Disconnected — reconnecting*. Use a **template SF Symbol with state color** for the bar item (e.g. `dot.radiowaves.left.and.right` states). Map `CBManager.state` cases explicitly instead of a default that means "scanning." *(`BeaconCentral.swift` state handling, `MenubarController.swift:54,86`)*
6. **Drive `ST_RECONNECTING` on the device** (or delete it everywhere to stop implying a state that never happens). Set it on disconnect-while-was-connected so the device shows "RECONNECTING" before falling to "HUB OFFLINE". *(`hub_task.cpp:37`)*
7. **Show "last synced HH:MM" on HUB OFFLINE** as the design intended — the age data already exists. *(`state_view.h` / buddy + usage views)*
8. **Hide the pairing hint once paired**; show it only when unpaired/scanning. *(`MenubarController.swift:22`)*
9. **Live-tick the last-sync age** (or stamp absolute time) so it can't read falsely stale. *(`MenubarController.swift:60`)*
10. **Re-model the buddy idle/activity state to CC hook semantics** (§3.5b). Concretely: drive `running` from `SessionStart` minus `SessionEnd` (not per-turn `Stop`), or track a live set of `session_id`s; either populate `waiting` from a real `Notification`-resolved pairing or drop it from the line; **label `TOK`/`CTX %` with the session they belong to** (or aggregate explicitly) instead of last-writer-wins; rename `Stop` entries from "session stopped" to "turn done"/"idle"; and **tag every activity entry and permission prompt with `cwd`/session** so concurrent A/B/C sessions are distinguishable and you can tell which one is asking. The hook payload already carries `session_id`/`cwd`/`transcript_path`. *(`ClaudeCodeBridge.swift:178-266,160`, buddy records + views)*
11. **Make a pending prompt impossible to miss** (§3.5c). Fastest, zero-risk path: have the **Mac hub play a sound / post a notification** the instant a prompt arrives (reuses the alert surface from Tier-0 #2, no firmware change, no coexistence cost). Later, an **optional, subtle device chime** via the ES8311 codec — gated on heap-headroom verification and shipped **opt-in with a quiet/off setting** (the audio analog of the reduced-motion rule), to stay within the "calm precision instrument" brand. *(`AppDelegate.swift` / `ClaudeCodeBridge.swift:160`; firmware audio = Explore-tier)*

### Tier 2 — onboarding & lifecycle
10. **A minimal first-run window** (not full wizard): three checkmarks — *Bluetooth ✓/✗*, *Claude Code hooks installed ✓/✗ (Install button)*, *Device paired ✓/✗* — with a one-line fix action per row. This is the single highest-impact addition for a non-author user; it replaces "read the README and hand-edit JSON." Grounded in IoT/smart-device onboarding guidance (visual, step-by-step, real-time status, clear remediation).
11. **Register a login item** (`SMAppService`) with a menu toggle, so the hub survives reboot.
12. **Graceful quit drain** — resolve held prompts as deny-with-reason before terminating, so in-flight CC calls get a clean answer.
13. **Add a "Forget device / re-pair" menu action** (the deferred P2-F item) for when bonding gets into a bad state — today there's no recovery path without OS-level Bluetooth surgery.

### Tier 3 — polish & honesty cleanups
14. Normalize touch targets across buddy themes (add `ext_click_area` to HUD). *(`buddy_hud.cpp`)*
15. Either populate `BuddyState.waiting` or drop it from the telemetry line so it's not a permanent `0`.
16. Add a device picker (or at least a name suffix in the menu) for the multi-`Beacon*` case rather than first-to-advertise-wins.
17. Finish the token-refresh-on-401 stubs so expiry self-heals instead of needing manual re-login.

---

## 5. The one tension worth a decision

**Should the Mac be able to approve/deny too, not just the device?** Today approval is device-only by design (keeps the decision physically at the desk, on-theme). But it means: device asleep/disconnected = silent auto-deny. Options:
- **(A) Keep device-only**, but make the auto-deny loud (Tier-0 #2). Lowest effort, preserves product intent.
- **(B) Add a Mac fallback prompt** (notification with Approve/Deny actions) when the device can't show it. Best resilience; adds a second approval surface to keep secure (must reuse the same id-matching + fail-closed cap).
- **(C) Hybrid:** device-first, Mac-notification fallback only when device is unreachable.

Recommend **(A) now** (cheap, removes the silent failure) and consider **(C)** later. Flagging for the user — this is a product/security call, not just UX.

---

## Appendix — perspective scorecard

| Perspective | Score | Note |
|---|---|---|
| Honesty / data trust | ●●●●● | model is the product's strongest asset |
| Security (steady state) | ●●●●○ | strong; weak spot is *visibility* of denials |
| Speed (happy path) | ●●●●○ | auto-reconnect is great once bonded |
| Easiness (first run) | ●○○○○ | terminal + hand-edited JSON + broken default path |
| Discoverability (failures) | ●○○○○ | everything is "Scanning…" |
| Error recovery | ●●○○○ | auto-retry good; dead-ends + silent kills bad |
| Lifecycle (restart/persist) | ●●○○○ | no login item, no forget-bond |

**Sources (research grounding):** [NN/g smart-device onboarding](https://www.nngroup.com/articles/smart-device-onboarding/) · [grandcentrix IoT onboarding](https://grandcentrix.net/en/blog/iot-onboarding/) · [AppCoda macOS status bar apps](https://www.appcoda.com/macos-status-bar-apps/) · [openclaw exec-approval UX issue #46708](https://github.com/openclaw/openclaw/issues/46708) · [openclaw approval-blocks-remote #58752](https://github.com/openclaw/openclaw/issues/58752)
</content>
</invoke>
