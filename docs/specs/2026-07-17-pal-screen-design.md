# PAL mascot: swipe-up/down overlay on the CLAUDE screen

A full-screen animated pixel-art mascot overlay (`firmware/src/ui/pal_panel.cpp`) whose
animation reflects Coding Buddy state. Opened by swiping up or down on the CLAUDE/buddy
carousel screen; swiping up or down again inside the overlay closes it back to the
carousel. Ported from [HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter),
a sibling ESP32-S3 + LVGL project targeting the same 2.16" AMOLED panel.

Not a carousel screen (it was, briefly, in an earlier revision of this feature — see git
history if curious): it's a modal overlay drawn on `lv_layer_top()`, following the same
pattern as `wifi_panel.cpp`/`theme_panel.cpp`/etc., just opened by a gesture instead of a
settings-row tap.

## Licensing caveat

Clawdmeter's own README states its mascot art is Anthropic's copyrighted "Clawd"
character, used without the company's permission. Beacon uses the same sprite assets
as-is. This was flagged to and explicitly accepted by the project owner (2026-07-17) —
recorded here so it isn't silently forgotten by a future contributor. If that changes,
the animation *mechanism* (indexed-sprite + zoom, state-driven selection) is reusable
with different/original art; only `pal_frames.h`/`pal_frames.c` would need replacing.

## Opening and closing (gesture)

`screen_buddy.cpp` hand-expands the `SCREEN_MODULE_SIMPLE` macro (rather than using it
directly, like `home`/`finance`/`usage`/`settings` still do) so it can register an
`LV_EVENT_GESTURE` handler on the CLAUDE screen's `page` after the per-theme view builds
it. A `LV_DIR_TOP`/`LV_DIR_BOTTOM` gesture calls `pal_panel_open()`; inside the overlay,
`pal_panel.cpp` registers its own gesture handler on its full-screen root, calling
`pal_panel_close()` the same way.

**Why this needed no changes to any of the 7 per-theme buddy views:** every plain
`lv_obj` defaults to `LV_OBJ_FLAG_GESTURE_BUBBLE` when it has a parent (`lv_obj.c`'s
`lv_obj_constructor`), so a swipe that starts on a session row, the question card, or an
approve/deny button bubbles up through its ancestors exactly like one that starts on
empty page background — *unless* something along the way opts out. `screen_buddy.cpp`
clears `GESTURE_BUBBLE` on `page` itself, which is what makes `page` the landing point
for a bubbled gesture starting anywhere in its subtree, with zero per-row/per-view
changes.

**Why a swipe can't also fire a stray tap** (e.g. auto-approving whatever permission
button the swipe happened to start on): LVGL sends `LV_EVENT_CLICKED` on release
whenever the touch wasn't captured by *scroll* — it does **not** check whether a
*gesture* already fired first, so without a guard, opening/closing the overlay would
also click through to whatever was under the finger. Both gesture handlers call
`lv_indev_wait_release(lv_indev_get_act())` before acting, which is LVGL's documented
way to say "this touch is handled, suppress its upcoming release/click."

Vertical gestures never race the horizontal carousel scroll: `lv_indev_scroll`'s
`find_scroll_obj()` only captures the carousel's pager as `scroll_obj` for a
*horizontal*-dominant drag (`lv_obj_set_scroll_dir(s_pager, LV_DIR_HOR)` in
`carousel.cpp`), and LVGL's own gesture detection (`indev_gesture()`) is unconditionally
skipped whenever `scroll_obj` is already set. A vertical drag never sets it, so gesture
detection always runs cleanly for the up/down swipe.

## State → animation mapping

Computed from `ds_get_buddy()` (`buddy_rec_t`, `firmware/src/core/records.h`) by
`pal_pick_state()` in `firmware/src/ui/screens/pal_state.h` (unchanged by the
carousel-screen → overlay migration — this file has zero LVGL/Arduino dependencies, see
`test/test_pal_state/`). Precedence, high to low:

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

Tapping anywhere in the overlay (that isn't a swipe) plays a one-shot animation on top
of whatever's currently looping, then hands back control to the normal state animation
once it finishes a single pass (`s_one_shot` flag in `pal_panel.cpp`, checked in
`advance_frame()`). Mapped per base state at the moment of the tap:

| Base state | Reaction |
|---|---|
| IDLE | `expression_surprise` |
| ACTIVE | `dance_djmix` |
| SLEEP | `work_think` |
| NOTIFY | none (tap ignored — the pulsing border already has the user's attention) |

If the mascot's real state changes while a reaction is still playing (e.g. a session
starts working mid-reaction), the state change wins immediately (`check_state()` clears
`s_one_shot` on any `st != s_last_state` transition).

**Click target is the whole overlay, not just the sprite:** `lv_img`'s hit box (even with
`LV_OBJ_FLAG_ADV_HITTEST`, which is zoom-aware) is still only the sprite's ~240px drawn
area — most taps on a 466px screen would miss. `s_img` clears `LV_OBJ_FLAG_CLICKABLE`;
the full-screen root (`s_root`) owns the click handler instead, so any tap in the overlay
triggers the reaction.

## Status label

A short status word renders bottom-center: `SLEEPING` / `WAITING` / `THINKING` /
`NEEDS YOU` (for SLEEP/IDLE/ACTIVE/NOTIFY respectively). Uses the shared `S.slot` style
(mono font, `ink_dim`) like other screens' status text. Only updated on state
transitions, not on tap reactions or idle rotation ticks.

## Rendering

Frames are 20×20px pixel art, source-indexed in Clawdmeter's JSON (≤10 colors) but
**pre-expanded at generation time** (`gen_pal_frames.py`) to `LV_IMG_CF_TRUE_COLOR_ALPHA`
(RGB565 + 8-bit alpha, 3 bytes/pixel = 1200 bytes/frame). Rendered via a single `lv_img`
object + `lv_img_set_zoom()` (≈12× to ~240px on screen, antialias off to keep pixel edges
crisp).

**Why not `LV_IMG_CF_INDEXED_8BIT` directly** (the first cut of this shipped that way and
rendered nothing visible on hardware): LVGL 8.4's zoom/rotate transform (`lv_draw_img.c:
decode_and_draw`) only takes the whole-buffer fast path — the one that actually applies
`draw_dsc->zoom` — when the decoder hands back a direct `img_data` pointer, which
`lv_img_decoder_built_in_open` only does for `TRUE_COLOR*`/`RGB565A8`/`ALPHA_8BIT`.
Indexed images always return `img_data == NULL` (decoded per pixel via a palette lookup
instead), which routes through the line-by-line fallback decode path — and that path has
**no zoom support at all**, so an indexed sprite silently renders at its native 20×20
size instead of scaling up. Pre-expanding to `TRUE_COLOR_ALPHA` sidesteps this entirely;
the tradeoff is ~4x the flash footprint (still under 600KB total for all 11 animations,
trivial against the 3MB app partition).

`s_img_dsc.data` is simply repointed at the current animation's current frame — each
frame is its own `const` array in flash (`pal_frames.c`), so no per-frame `memcpy` is
needed. This is safe because `LV_IMG_CACHE_DEF_SIZE` is 0 (`lv_conf.h`): the decoder
re-reads `((lv_img_dsc_t*)src)->data` fresh on every draw rather than caching a stale
pointer from the first open, so `lv_obj_invalidate()` after repointing is enough — no
`lv_img_set_src()` churn.

Frame advance is driven by a dedicated `lv_timer_t`, created on `pal_panel_open()` and
deleted on `pal_panel_close()` (re-armed each frame to that frame's `holds_ms` value).
The same timer also polls `ds_get_buddy()` each tick (`check_state()`, folded in rather
than kept as a separate carousel-style `update()` — there's no carousel tick driving this
overlay anymore, since it isn't a carousel module). The whole callback no-ops while the
panel is dimmed/asleep (`idle_is_inactive()`), so the overlay doesn't keep the panel
awake or redraw while the display should be sleeping (same rationale as issue #60's
carousel tick-pause).

## Notification border

A full-bleed `lv_obj` with `radius=70`, `border_color=theme->alert`, opacity pulsed
40%↔100% via `lv_anim_t` (period ~1100ms, `LV_ANIM_REPEAT_INFINITE`,
`lv_anim_path_ease_in_out`). Hidden and un-started except during `PAL_STATE_NOTIFY`;
started/stopped only on state *transition* (not every `check_state()` tick) via a
`s_last_state` guard.

## Regenerating the sprite data

`firmware/src/ui/screens/pal_frames.h`/`.c` are generated, not hand-written. See
`firmware/tools/gen_pal_frames.py`'s docstring: fetch the 11 relevant JSON files (the
original 10 plus `dance_djmix`, used for the ACTIVE-state tap reaction) from
Clawdmeter's `tools/claudepix_data/`, then run the script against that directory.
