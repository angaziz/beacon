# PAL screen: animated agent mascot

A 6th carousel screen (`firmware/src/ui/screens/screen_pal.cpp`, id `"PAL"`) showing an
animated pixel-art mascot whose animation reflects Coding Buddy state. Ported from
[HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter), a sibling
ESP32-S3 + LVGL project targeting the same 2.16" AMOLED panel.

## Licensing caveat

Clawdmeter's own README states its mascot art is Anthropic's copyrighted "Clawd"
character, used without the company's permission. Beacon uses the same sprite assets
as-is. This was flagged to and explicitly accepted by the project owner (2026-07-17) —
recorded here so it isn't silently forgotten by a future contributor. If that changes,
the animation *mechanism* (indexed-sprite + zoom, state-driven selection) is reusable
with different/original art; only `pal_frames.h`/`pal_frames.c` would need replacing.

## State → animation mapping

Computed from `ds_get_buddy()` (`buddy_rec_t`, `firmware/src/core/records.h`) by
`pal_pick_state()` in `firmware/src/ui/screens/pal_state.h`. Precedence, high to low:

| Precedence | Condition | State | Animation |
|---|---|---|---|
| 1 | `prompt.present` | NOTIFY | `dance_bounce`, looped + pulsing 70px-radius `alert`-color border |
| 2 | `hdr.state == ST_HUB_OFFLINE` or `session_count == 0` | SLEEP | `expression_sleep`, looped |
| 3 | any `sessions[i].state == BST_WORKING` | ACTIVE | `work_coding`, looped |
| 4 | otherwise (has agent, nothing pending/working) | IDLE | rotates every 8s through `idle_breathe → idle_blink → idle_look_around → expression_wink → dance_sway` |

A pending prompt outranks an actively-working session because it needs the user's
response right now — the mascot shouldn't look "business as usual" while something is
blocked on them. `expression_surprise` and `work_think` are intentionally excluded from
the idle rotation pool — they're reserved as tap reactions (see below).

## Tap reaction (one-shot)

Tapping the mascot plays a one-shot animation on top of whatever's currently looping,
then hands back control to the normal state animation once it finishes a single pass
(`s_one_shot` flag in `screen_pal.cpp`, checked in `advance_frame()`). Mapped per base
state at the moment of the tap:

| Base state | Reaction |
|---|---|
| IDLE | `expression_surprise` |
| ACTIVE | `dance_djmix` |
| SLEEP | `work_think` |
| NOTIFY | none (tap ignored — the pulsing border already has the user's attention) |

If the mascot's real state changes while a reaction is still playing (e.g. a session
starts working mid-reaction), the state change wins immediately (`update()` clears
`s_one_shot` on any `st != s_last_state` transition).

**Hit-test gotcha:** `lv_img`'s default click area is its *unscaled* source size
(20×20px, set in `lv_img_set_src`'s `lv_obj_refresh_self_size`) — `lv_img_set_zoom` only
expands the *draw* area, not the input hit box. Without `LV_OBJ_FLAG_ADV_HITTEST`, taps
anywhere on the visually ~240px mascot outside that tiny centered 20×20 box are silently
ignored. `s_img` sets `LV_OBJ_FLAG_ADV_HITTEST` alongside `LV_OBJ_FLAG_CLICKABLE` so
LVGL's `LV_EVENT_HIT_TEST` path (`lv_img.c`) uses the actual zoomed/rotated area instead.

## Status label

A short status word renders bottom-center, above the carousel's dot indicator:
`SLEEPING` / `WAITING` / `THINKING` / `NEEDS YOU` (for SLEEP/IDLE/ACTIVE/NOTIFY
respectively). Uses the shared `S.slot` style (mono font, `ink_dim`) like other
screens' status text. Only updated on state transitions, not on tap reactions or idle
rotation ticks.

## Rendering

Frames are 20×20px pixel art, source-indexed in Clawdmeter's JSON (≤10 colors) but
**pre-expanded at generation time** (`gen_pal_frames.py`) to `LV_IMG_CF_TRUE_COLOR_ALPHA`
(RGB565 + 8-bit alpha, 3 bytes/pixel = 1200 bytes/frame). Rendered via a single `lv_img`
object + `lv_img_set_zoom()` (≈12× to ~240px on screen, antialias off to keep pixel edges
crisp).

**Why not `LV_IMG_CF_INDEXED_8BIT` directly** (the first cut of this screen shipped that
way and rendered nothing visible on hardware): LVGL 8.4's zoom/rotate transform
(`lv_draw_img.c: decode_and_draw`) only takes the whole-buffer fast path — the one that
actually applies `draw_dsc->zoom` — when the decoder hands back a direct `img_data`
pointer, which `lv_img_decoder_built_in_open` only does for `TRUE_COLOR*`/`RGB565A8`/
`ALPHA_8BIT`. Indexed images always return `img_data == NULL` (decoded per pixel via a
palette lookup instead), which routes through the line-by-line fallback decode path —
and that path has **no zoom support at all**, so an indexed sprite silently renders at
its native 20×20 size instead of scaling up. Pre-expanding to `TRUE_COLOR_ALPHA` sidesteps
this entirely; the tradeoff is ~4x the flash footprint (still under 600KB total for all
10 animations, trivial against the 3MB app partition).

`s_img_dsc.data` is simply repointed at the current animation's current frame — each
frame is its own `const` array in flash (`pal_frames.c`), so no per-frame `memcpy` is
needed. This is safe because `LV_IMG_CACHE_DEF_SIZE` is 0 (`lv_conf.h`): the decoder
re-reads `((lv_img_dsc_t*)src)->data` fresh on every draw rather than caching a stale
pointer from the first open, so `lv_obj_invalidate()` after repointing is enough — no
`lv_img_set_src()` churn.

Frame advance is driven by a dedicated 100ms-poll `lv_timer_t` (re-armed each frame to
that frame's `holds_ms` value) rather than the carousel's shared 500ms tick, which is too
coarse for blink/breathe animation. The timer body only does work when
`carousel_current() == PAL_SCREEN_INDEX` and `!idle_is_inactive()` — otherwise it's a
cheap no-op, so PAL doesn't keep the panel awake or redraw while scrolled off-screen or
while the display is dimmed/asleep (same rationale as issue #60's tick-pause).

## Notification border

A full-bleed `lv_obj` with `radius=70`, `border_color=theme->alert`, opacity pulsed
40%↔100% via `lv_anim_t` (period ~1100ms, `LV_ANIM_REPEAT_INFINITE`,
`lv_anim_path_ease_in_out`). Hidden and un-started except during `PAL_STATE_NOTIFY`;
started/stopped only on state *transition* (not every `update()` tick) via a
`s_last_state` guard. Scoped to the PAL screen only — not a global top-layer overlay
across all screens (a possible future enhancement, not this one).

## Regenerating the sprite data

`firmware/src/ui/screens/pal_frames.h`/`.c` are generated, not hand-written. See
`firmware/tools/gen_pal_frames.py`'s docstring: fetch the 11 relevant JSON files (the
original 10 plus `dance_djmix`, used for the ACTIVE-state tap reaction) from
Clawdmeter's `tools/claudepix_data/`, then run the script against that directory.
