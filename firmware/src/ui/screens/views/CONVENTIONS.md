# Per-theme screen views — implementation conventions (READ FIRST)

You implement bespoke per-theme LAYOUTS for Beacon screens (ESP32-S3, LVGL 8.4, Arduino). Each file
is one (screen x theme) "view". The visual target is `docs/design/mockups/directions.html` (per-theme
Home + AI-Usage); for the other screens, take the content structure from the editorial lane in
directions.html. Match your theme's lane closely; extend that visual language to the other screens.

## File contract
File: `src/ui/screens/views/<screen>_<theme>.cpp`, where screen in
{home,finance,usage,buddy,settings}, theme in
{editorial,hud,calm,blueprint,led,oscilloscope,analog}.

Each file is self-contained with file-static widget pointers and MUST end with exactly:
```cpp
const screen_view_t <screen>_<theme>_view = { build, update };
```
where `static void build(lv_obj_t* page)` lays out the design and `static void update(void)` refreshes
from the DataStore. `screen_view_t` is in `ui/screen.h`.

Skeleton:
```cpp
#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"
#include <Arduino.h>     // millis()
// + ui/fmt.h (finance), ui/gauge.h, hal/display.h (settings brightness), ui/theme_catalog.h (THEME_COUNT)

static lv_obj_t *s_slot, *s_x; /* widget refs */
static void build(lv_obj_t* page) { /* create objects, store in statics */ }
static void update(void) { /* mutate text/values/visibility only; never create here */ }
const screen_view_t home_hud_view = { build, update };
```

## Rules
- The page BACKGROUND + theme chrome (grid/dots/graticule/etc.) is ALREADY drawn by the carousel
  before build() runs. Do NOT set the page bg or draw the background. Only add content on top.
- Screen is 466x466, ROUND. Keep all content inside `SAFE_INSET` (40px). Nothing critical in the
  corner arcs. SCREEN_W==SCREEN_H==466.
- Colors/fonts come from the active theme: `const beacon_theme_t* t = theme_active();`
  fields: bg, ink, ink_dim, line, accent, accent2, up, down, alert (all lv_color_t);
  f_hero (oversized figures), f_display (titles/figures), f_body, f_mono (labels/eyebrows).
  You MAY use the prebuilt shared styles in `S` (styles.h): S.eyebrow(mono+accent), S.slot(mono+ink_dim),
  S.display, S.hero, S.body, S.up, S.down, S.accent, S.hairline(bg=line), S.dim(ink_dim). Mixing
  `lv_obj_add_style(o,&S.x,0)` and direct `lv_obj_set_style_*` with theme tokens is fine here (each
  view is per-theme). Set styles in build(); in update() only change text/values/show-hide.
- update() is idempotent and read-only w.r.t. layout. Read snapshots: ds_get_weather(), 
  ds_get_finance_count()/ds_get_finance(i), ds_get_usage(), ds_get_buddy().
- now: call the global `now_s()` (declared in ui/screen.h, defined in core/timekeep.cpp).
- STATE handling (every data screen): use state_view.h helpers to reflect loading/live/stale/offline/
  error/hub-offline. Minimum: a small status chip (theme f_mono) in a corner showing
  `sv_status(buf,n,&rec.hdr,now)` text when non-live; dim the values when `sv_dim(state)`; show
  placeholder "--" when `sv_placeholder(state)`. Keep it consistent with your theme's look.
- Time-dependent fields (clock, date, week, reset times) have NO real source in this chunk — render
  placeholders (`--:--`, `--`). EXCEPTION: for the analog theme home clock, draw a STATIC pleasant
  hand position (e.g. ~10:08) so the face reads well; it is not driven by real time yet.
- Transparent containers: `lv_obj_create` then `lv_obj_remove_style_all(o)` for layout boxes (rows/
  flex). Bars (lv_bar) need explicit `lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN/INDICATOR)`
  + bg_color (default theme is OFF, so unstyled parts are invisible).

## Custom-draw (analog clock, concentric rings, scope traces, conic sub-dials, dimension axes)
Use a custom draw on an `lv_obj` (transparent, sized, positioned), drawing in DRAW_MAIN:
```cpp
static void draw_cb(lv_event_t* e){
  lv_obj_t* o = lv_event_get_target(e);
  lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(e);
  lv_area_t a; lv_obj_get_coords(o, &a);   // SCREEN-ABSOLUTE coords
  const beacon_theme_t* t = theme_active(); if(!t) return;
  lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld); ld.color=t->accent; ld.width=2; ld.opa=LV_OPA_COVER;
  lv_point_t p1={a.x1,a.y1}, p2={a.x2,a.y2}; lv_draw_line(ctx,&ld,&p1,&p2);
  // lv_draw_arc(ctx,&arc_dsc,cx,cy,radius,start_angle,end_angle); lv_draw_rect(ctx,&rect_dsc,&area);
}
// in build: lv_obj_add_event_cb(o, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
// to refresh dynamic custom-draw in update: lv_obj_invalidate(o);
```
Confirmed-present 8.4 APIs: lv_event_get_draw_ctx, ctx->clip_area, lv_draw_line(+lv_draw_line_dsc_t),
lv_draw_rect(+lv_draw_rect_dsc_t, radius=LV_RADIUS_CIRCLE for dots), lv_draw_arc(+lv_draw_arc_dsc_t),
lv_obj_get_coords. For gauges you may instead reuse `gauge_render(parent, t, pct)` from ui/gauge.h.

## Per-screen content + record fields
- home (HOME eyebrow): clock(placeholder `--:--`) + date(placeholder) + weather temp_c (`%.1f` + degree
  `\xC2\xB0`), humidity_pct (`%.0f%%`), condition (leave blank/placeholder). Weather state via hdr.
  ANALOG theme: replace digital clock with a drawn analog face (hands+ticks+hub); keep temp/humidity readouts.
- finance (MARKETS): rows over ds_get_finance(0..count-1): id (char[]), value (double -> ui/fmt.h
  fmt_value), change_pct (double -> fmt_change returns +1/-1/0 -> up/down color + ^/v glyph). Per-row hdr
  state. >6 rows: vertical-scroll list inside the page (lv_obj_set_scroll_dir(list, LV_DIR_VER)).
- usage (LIMITS): data-driven providers, up to 4 on the wire, first 2 rendered (2x2 grid). usage_rec_t:
  u.count + u.p[i]{ char id[13]; char label[11]; usage_window_t h5,d7; bool stale } . usage_window_t
  {int16_t pct; uint32_t reset}. Render from u.count + u.p[i].label (NOT hardcoded provider names);
  helpers usage_slot/usage_tag/usage_abbr2/usage_none in screen_common.h. count 0 => all placeholders;
  count 1 => provider 0 in the top row; >2 => first 2. pct<0 == unavailable => show "--" and NO bar/ring
  fill (never feed -1 to a 0..100 widget). resets => placeholder. Whole-screen HUB_OFFLINE via u.hdr.
  Casing per theme: lowercase tags (calm/analog), uppercase (editorial/blueprint/led/oscilloscope/hud).
  Per directions.html: HUD=concentric rings, Calm=big-fig + dots, Blueprint=measure axis+marker,
  LED=cell meter, Oscilloscope=scope level fill, Analog=conic sub-dials, Editorial=track bars.
- buddy (screen id "CLAUDE", branded "AGENTS"): status line (running, waiting, tokens/1000 +"K", context_pct). Discriminate
  buddy.prompt.present: true => prompt layout (prompt.tool, prompt.hint in a box, DENY|APPROVE);
  false => idle (entries[0..entry_count-1], or "idle"). Approve/Deny tap is a LOCAL STUB: read rec,
  set prompt.present=false, ds_set_buddy(&rec), LOGI. Disable actions when hdr.state is HUB_OFFLINE.
- settings (SETTINGS): rows Wi-Fi(status text "not set"), Brightness, Theme, Sleep("5 min"),
  About(">"). INTERACTIVE: Theme row tap cycles theme, Brightness row tap
  cycles 40/60/80/100%. NEVER call theme_set directly in the event (it deletes this very object mid-event)
  => defer with lv_async_call:
  ```cpp
  static void do_next_theme(void*){ theme_set((theme_index()+1)%THEME_COUNT); }
  // in tap handler: lv_async_call(do_next_theme, NULL);
  ```
  Brightness: `#include "hal/display.h"`; display_brightness(uint8_t 0..255); safe to call inline.

## Don't
- Don't draw the page background/chrome. Don't create objects in update(). Don't call theme_set inline
  from an event. Don't feed pct<0 to bars/arcs. Don't exceed SAFE_INSET. ASCII only in comments (`=>`).
