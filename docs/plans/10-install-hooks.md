# Implementation Plan — Issue #10: Fix hard-coded statusline path + add `install-hooks` installer

Hub-only (bash + jq tooling). Source: audit §3.1. jq 1.7.1 present.

## Context
- `build-app.sh`: `set -euo pipefail`, `cd "$(dirname "$0")"`, positional subcommand parse (`run`). `install-hooks` must intercept BEFORE `swift build` (no Swift toolchain needed).
- Shim `statusline-shim/beacon-statusline` already does forward-then-delegate (`printf '%s' "$input" | "$@"`). WRAP = make the shim's argv tail the user's old command. No shim change.
- Snippet hard-codes `statusLine.command` (line 41), has 4 hook events each with one `{type:http,url:http://127.0.0.1:8765/hook,timeout:N}`. `_comment` must be stripped before merge.
- This user's `~/.claude/settings.json` is the idempotency fixture: ALL beacon hooks already present + non-beacon entries (notify.sh, cache-heal.mjs) + already-wrapped statusLine (`.../beacon-statusline bash ~/.claude/statusline-command.sh`). Re-run must be a semantic no-op. Some hand-installed beacon entries lack a `matcher`, so the idempotency key must be the INNER hook object (`type`+`url`), not the wrapper's matcher.

## D1 — Snippet placeholder
`hub/claude-code-settings.snippet.json:41`: replace the hard-coded author path with literal token `"command": "__BEACON_SHIM__"`. The installer does NOT read this value (it computes the wrap itself) — the token is documentation + makes a hand-copied snippet obviously non-functional instead of silently pointing at the author's machine. Optionally update `_comment` to mention `./build-app.sh install-hooks`.

## D2 — install-hooks flow (in build-app.sh, intercept right after `cd "$(dirname "$0")"`)
INLINE the whole block inside the `if` (the file has no functions; inlining avoids a define-before-use ordering bug codex flagged — a function called before its definition is a bash command-not-found):
```
if [ "${1:-}" = "install-hooks" ]; then
  ... Steps 0-7 inline ...
  exit 0
fi
```
- **Step 0 dep check:** `command -v jq` or fail with `brew install jq`.
- **Step 1 shim path:** `SHIM="$PWD/statusline-shim/beacon-statusline"` (derives from dirname $0 => correct anywhere); ensure exists/executable.
- **Step 2 settings:** `SETTINGS="$HOME/.claude/settings.json"`; `mkdir -p ~/.claude`; bootstrap `{}` if absent.
- **Step 3 validate:** `jq -e . "$SETTINGS"` or abort (strict JSON; no JSONC) — no write.
- **Step 4 backup:** `cp` to `$SETTINGS.bak.$(date +%Y%m%d-%H%M%S)`.
- **Step 5 merge (single jq, shim via `--arg`, never shell-interpolated):** CODEX FIX — `def`s must precede the pipeline; `... | def x: ...; | def y: ...;` is a jq syntax error (`unexpected '|'`). Defs go first, no leading/trailing `|` around them:
```
jq -n --arg shim "$SHIM" --slurpfile cur "$SETTINGS" --slurpfile snip "claude-code-settings.snippet.json" '
  def is_beacon_inner: (.type=="http") and (.url=="http://127.0.0.1:8765/hook");
  def wrapper_is_beacon: (.hooks // []) | any(.[]; is_beacon_inner);
  ($cur[0]) as $s
  | ($snip[0] | del(._comment)) as $snippet
  | reduce ($snippet.hooks | keys_unsorted[]) as $ev ($s;
      ($snippet.hooks[$ev]) as $beaconWrappers
      | .hooks[$ev] = ((.hooks[$ev] // []) as $existing
          | if ($existing | any(.[]; wrapper_is_beacon)) then $existing
            else $existing + $beaconWrappers end))
  | (.statusLine.command // "") as $oldcmd
  | .statusLine = ( if ($oldcmd == $shim or ($oldcmd | startswith($shim + " ")))
                      then {type:"command", command:$oldcmd}            # already wrapped => leave (exact/boundary match)
                    elif ($oldcmd=="") then {type:"command", command:$shim}
                    else {type:"command", command:($shim+" "+$oldcmd)} end )'
```
  - Hooks idempotency: match the inner `{type:http,url:8765}` object anywhere in an event's wrappers; append `$existing + $beaconWrappers` only when absent. Existing non-beacon entries preserved; events not in snippet (PostToolUse, UserPromptSubmit...) never touched. (Append-only, NOT repair: an existing beacon hook with a stale `timeout` is left as-is — explicit, acceptable.)
  - statusLine wrap (CODEX FIX — exact/boundary match, not bare `startswith`, so `${shim}-old` is NOT falsely treated as wrapped): already-wrapped (`$oldcmd == $shim` or starts with `"$shim "`) => leave intact (keeps delegate tail, no double-wrap), forcing `type:"command"` (our shim is a command, so an already-wrapped-but-mistyped entry is repaired); empty => bare shim; else => `"<shim> <oldcmd>"` (shim's `"$@"` runs the user's renderer verbatim, multi-arg ok).
- **Step 6 atomic write + post-validate:** `TMP=$(mktemp "$HOME/.claude/settings.json.XXXXXX")`; `trap 'rm -f "$TMP"' EXIT`; run jq into `$TMP`; `jq -e . "$TMP"` validate; **`chmod 644 "$TMP"`** (CODEX FIX — mktemp creates 0600; restore the 0644 settings perms before replacing); `mv "$TMP" "$SETTINGS"`; `trap - EXIT`. mktemp same-dir => atomic rename, no partial write. Reading `$SETTINGS` via slurpfile completes before output, so read==write path is safe. (Concurrent external edit between read and mv is out of scope — settings isn't edited during install.)
- **Step 7 report:** print settings path, shim, backup path, "restart Claude Code".

## D3 — Before/after (this user)
All 4 beacon events: `wrapper_is_beacon` TRUE => no append; notify.sh/cache-heal preserved. statusLine starts with $SHIM => left intact (delegate tail kept). PostToolUse etc untouched => semantic no-op.
Fresh (`{}`): 4 events appended; statusLine = bare shim => working usage, zero manual edit.
Custom statusline (`bash ~/.claude/my.sh`): => `"<shim> bash ~/.claude/my.sh"` (wrapped, not replaced).

## D4 — Edge cases
no settings.json (bootstrap {}); no jq (fail w/ brew msg); invalid/JSONC (abort, no write); already-wrapped (no double-wrap); beacon present (no dup, inner-object key); multi-arg statusline (concat, shim `"$@"`); non-beacon hooks (append-only preserves); hand-installed beacon w/ different/no matcher (inner-object key still matches); shim path with spaces (safe in jq via --arg; warn that the space-joined statusLine string may not parse at the CC layer; don't block); merge/output invalid (post-validate abort, original untouched).

## D5 — Verify (on COPIES / fake HOME; never clobber real file in dev)
1. Idempotency vs real state: `cp ~/.claude/settings.json /tmp/s0.json`; run jq PROGRAM s0=>s1, s1=>s2; `diff <(jq -S . /tmp/s1.json) <(jq -S . /tmp/s2.json)` empty => idempotent; `jq '.hooks.PermissionRequest|map(.hooks[]|select(.url=="http://127.0.0.1:8765/hook"))|length' /tmp/s1.json` == 1; statusline single shim occurrence. `diff <(jq -S . s0) <(jq -S . s1)` => real-state no-op. (First write vs already-installed is byte-different but semantically equal — jq reserializes; the run-twice byte-identity is pass2==pass3.)
2. Fresh: `echo '{}'` => 4 events each one http-8765; statusLine == bare shim.
3. Custom wrap: statusLine `bash /Users/me/.claude/my.sh` => `"<shim> bash ..."`.
4. Sandbox HOME e2e: `HOME=/tmp/fakehome build-app.sh install-hooks` twice; diff => idempotent; backup created.

## D6 — Risks
clobber (append-only + backup + atomic + post-validate); jq quoting (vars via --arg/--slurpfile only, no $() in program); spaces (warn, don't block); partial writes (mktemp same-dir + validate + mv + trap); read==write (slurpfile reads fully first); JSONC (Step 3 abort); jq features all >= 1.6.

## Sequencing
1. Snippet line 41 => `"__BEACON_SHIM__"` (+ _comment). 2. Add install-hooks block after `cd "$(dirname "$0")"` (before RUN/swift build), Steps 0-7, terse commented bash, guard cp/mkdir under set -e. 3. Update usage header to document install-hooks. 4. Verify per D5 against copies/fake HOME only.
