# Font asset manifest (P0-B)

Glyph-subset LVGL fonts for the 7-theme engine. Generated C arrays (`font_*.c`) are committed and
link into the **app slot (~3 MB `ota_0`)**, not the data partition. Total generated C: ~980 KB
(GC-dropped until referenced; real linked cost is measured by the Task 9 demo build).

## Sources (google/fonts, OFL) + weights

Variable fonts instanced to static weights via `fonttools varLib.instancer`.

| Family | google/fonts path | Instanced weight |
|---|---|---|
| Space Grotesk | `ofl/spacegrotesk/SpaceGrotesk[wght].ttf` | wght=500 |
| Rajdhani | `ofl/rajdhani/Rajdhani-Medium.ttf` | static Medium |
| Doto | `ofl/doto/Doto[ROND,wght].ttf` | wght=500, ROND=0 |
| Chakra Petch | `ofl/chakrapetch/ChakraPetch-Medium.ttf` | static Medium |
| Pixelify Sans | `ofl/pixelifysans/PixelifySans[wght].ttf` | wght=400 |
| JetBrains Mono | `ofl/jetbrainsmono/JetBrainsMono[wght].ttf` | wght=500 |
| Inter | `ofl/inter/Inter[opsz,wght].ttf` | wght=400, opsz=14 |

## Roles, sizes, glyph subsets (lv_font_conv@1.5.3, --bpp 4)

| Role | Size px | Glyphs | Files |
|---|---|---|---|
| hero | 84 | `0-9 : % . , + - / ° space` | one per display family (7) |
| display | 30 | printable ASCII `0x20-0x7E` + `0xB0` | one per display family (7) |
| body | 18 | printable ASCII + `0xB0` | sg, raj, cp, inter, jbm (5) |
| mono | 15 | printable ASCII + `0xB0` | jbm (1, shared by all themes) |

## Per-theme mapping (theme.cpp `THEME_FONTS[]`)

| Theme | hero / display | body | mono |
|---|---|---|---|
| editorial | Space Grotesk | Space Grotesk | JetBrains Mono |
| hud | Rajdhani | Rajdhani | JetBrains Mono |
| calm | Doto | Inter | JetBrains Mono |
| blueprint | Chakra Petch | Chakra Petch | JetBrains Mono |
| led | Pixelify Sans | Inter | JetBrains Mono |
| oscilloscope | JetBrains Mono | JetBrains Mono | JetBrains Mono |
| analog | Inter | Inter | JetBrains Mono |

## Regenerate

```bash
# 1. download + instance to /tmp/beaconfonts/ttf/<Family>.ttf (see Sources table; fonttools instancer)
# 2. per family: lv_font_conv hero(84,symbols) + display(30,ascii); bodies(18,ascii); jbm mono(15,ascii)
npx -y lv_font_conv@1.5.3 --font <ttf> --size <px> --bpp 4 --format lvgl \
    [-r 0x20-0x7E -r 0xB0 | --symbols "0123456789:%.,+-/° "] -o font_<key>_<role>.c
```

Requires `-DLV_LVGL_H_INCLUDE_SIMPLE` in the build (generated files then `#include "lvgl.h"`).
Weights/hues are tunable on hardware (Task 9 demo); the frozen part is the theme ids + gauge mapping + `beacon_theme_t`.
