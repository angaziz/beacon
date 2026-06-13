# Contributing to Beacon

Thanks for the interest. Beacon is a personal project built in the open. It's early and
opinionated, so a few notes before you dig in.

## How this project is built

**Prototype-first.** Risky or unknown things get a throwaway spike on real hardware before
they get a spec. The design was settled by building real mockups and looking at pixels; the
architecture was settled by proving WiFi+BLE coexistence on the actual board. Specs follow
evidence, not the other way around. If you propose something non-trivial, a small spike that
shows it works beats a long argument.

**Read the context first.** Before changing direction, skim:
- `PRODUCT.md` — who it's for and the design principles
- `DESIGN.md` — the visual system and theme-token model (don't fork the look; extend tokens)
- `docs/research/` — what the hardware can/can't do and why decisions were made

## Repo areas

- `firmware/` — the product firmware (PlatformIO; build/flash guide in `firmware/README.md`). End users flash a released build with the [web flasher](https://angaziz.github.io/beacon/) (`firmware/flasher.html`, deployed to GitHub Pages on release); contributors build and flash from source with `pio run -e beacon -t upload`.
- `hub/` — the macOS menubar hub (SwiftPM; install/dev guide in `hub/README.md`).
- `docs/spikes/` — throwaway Arduino (C++) hardware spikes + setup/flashing guide (`docs/spikes/SETUP.md`), kept separate from the product firmware.
- `docs/design/mockups/` — HTML theme mockups. Iterate here, render with the Playwright helper
  in `docs/design/tooling/` (`node docs/design/tooling/shoot.mjs <file.html> <out-dir>`), then
  eyeball the PNGs. Keep new themes token-driven per `DESIGN.md`.
- `docs/` — research, design, and spikes.

## Ground rules

- **No secrets in commits.** WiFi creds and API tokens stay local — edit the placeholder
  constants in the sketches, don't commit them. `.gitignore` guards against committing credential files.
- **Don't commit build output or `node_modules`.** Already gitignored; don't force-add.
- **Match the surrounding style.** Firmware: keep the existing C++ conventions and pin/macro
  patterns. Surgical changes — every changed line should trace to the change you're making.
- **Themes are tokens, not rewrites.** A new theme = a token set + (rarely) one bespoke
  widget. See the theme catalog in `DESIGN.md`.
- **Honesty over polish in data.** Screens must show stale/offline/error states truthfully;
  never render a guessed value as live (see `DESIGN.md` → Screen states).

## Conventions

**Commits & PR titles.** Conventional Commits: `type(scope): subject`.
- Types: `feat` `fix` `docs` `refactor` `perf` `test` `chore` `ci` `build` `revert`.
- Scope (optional): `firmware` `hub` `docs` `ci` — omit when the change spans several.
- Subject: lowercase, imperative ("add", not "added"), no trailing period. Abbreviations and
  proper nouns keep their case: WiFi, BLE, CC, IMU, ESP32, AMOLED, SwiftUI.
- Breaking change: `type(scope)!: subject` or a `BREAKING CHANGE:` footer.
- A PR title becomes the squash-merge commit, so it follows the same rules. Link issues in the
  PR body (`Closes #N`), not in the title.

Examples: `feat(hub): add WiFi network at runtime`, `fix(firmware): smooth carousel wrap-around
swipe` — not `[FIRMWARE] fix swipe (#22)`, not `feat: Add WiFi Network.`

**Branches.** `<type>/<issue#>-<kebab-summary>`, e.g. `feat/36-usage-rows`, `fix/16-lifecycle`.
Drop the number when there's no issue.

**Issues & labels.** Title is an imperative phrase in sentence case — no `[Firmware]`/`[Hub]`
prefixes; the area label carries that. Label every issue with at least one area + one type,
plus a priority when known:
- area: `firmware`, `hub` (docs work uses `documentation`; CI/dependency bumps use the
  dependabot-managed `github_actions`/`dependencies` labels).
- type: `bug`, `enhancement`, `documentation`, `question`.
- priority: `P0` (breaks trust / no recovery), `P1` (significant friction), `P2` (polish).
- `epic` marks a tracking issue that spans several tasks.

**Versioning.** Semantic versioning, tagged per component (firmware and hub ship separately):
`firmware-vX.Y.Z` and `hub-vX.Y.Z`. `fix` => patch, `feat` => minor, breaking => major.
Pre-1.0, breaking changes may land in a minor bump. `docs/` ships no artifact and isn't tagged.
Pushing a `firmware-v*` or `hub-v*` tag triggers that component's release workflow.

## Workflow

1. Open an issue describing the change (especially for hardware-facing or architectural work).
2. Branch from `main`.
3. For firmware: confirm it compiles and runs on the board; note the board settings used. CI builds the device firmware and runs the host unit tests (`pio test -e native`) and the hub tests (`swift test`) on every PR.
4. For design: include rendered PNG(s) of the affected screen(s).
5. Open a PR with a clear description and any before/after evidence.

## Hardware note

Targets the Waveshare **ESP32-S3-Touch-AMOLED-2.16** (CO5300 QSPI display, AXP2101 PMU,
QMI8658 IMU). Other Waveshare AMOLED boards are similar but not identical (driver, touch,
panel size, button wiring differ) — porting is welcome but treat pins/rails as per-board.
