import Foundation

// Pure omp-extension logic (issue #136), split out of the executable so the managed extension source
// and its "is this the current managed file?" check are host-tested. The executable's HooksInstaller
// only does file IO (mkdir, back up an unrecognized file, atomic write) around this.
//
// Unlike Claude (settings.json jq merge) and Codex (config.toml managed block + trust hash), omp
// auto-discovers every module in ~/.omp/agent/extensions/, so installation is a single self-contained
// file. The file is wholly Beacon-managed: detection is exact-content equality, not a marker substring,
// so a truncated/edited/older-or-newer file reads as NOT current and Settings offers reinstall.
public enum OmpHooks {

    // The LocalIngestServer route the beacon.ts extension POSTs to.
    public static let routePath = "/omp/hook"

    // Filename written under ~/.omp/agent/extensions/.
    public static let extensionFileName = "beacon.ts"

    // Current iff the installed file is byte-identical (modulo surrounding-whitespace trim) to the
    // managed source. Substring/marker checks are rejected: a truncated file or a future
    // "beacon-omp v10" must read as NOT current so Settings offers reinstall.
    public static func isCurrent(_ content: String) -> Bool {
        content.trimmingCharacters(in: .whitespacesAndNewlines)
            == extensionSource.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    // The managed omp extension. This IS the wire contract with the hub's HookBuddyProvider on
    // /omp/hook: request {hook_event_name, session_id, cwd}. Every v4 event is fire-and-forget and the
    // hub answers {"ok":true} -- v4 sends no PermissionRequest, so nothing here can block a tool call
    // (CONTRACT.md §C.6 records why omp cannot gate the way the Claude/Codex hooks do).
    public static let extensionSource: String = #"""
// beacon-omp v4 -- managed by Beacon hub; do not edit (reinstall overwrites).
import type { ExtensionAPI } from "@oh-my-pi/pi-coding-agent";

const HUB = "http://127.0.0.1:8765/omp/hook";

export default function beacon(pi: ExtensionAPI) {
  pi.setLabel("Beacon Buddy");
  let sessionId = "";
  let cwd = "";

  const post = (body: Record<string, unknown>, timeoutMs: number) =>
    fetch(HUB, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ session_id: sessionId, cwd, ...body }),
      signal: AbortSignal.timeout(timeoutMs),
    });

  const lifecycle = (event: string, timeoutMs = 3_000) => {
    if (!sessionId) return Promise.resolve();
    return post({ hook_event_name: event }, timeoutMs).catch(() => {});
  };

  // (Re)bind identity on start/switch/branch: omp emits session_switch for new/resumed/forked
  // sessions and session_branch after branching; binding only on session_start would pin later
  // activity to a dead id. hasUI=false covers print mode AND task subagents: skip lifecycle binding
  // for those here, but the approval mirrors below MUST re-check ctx.hasUI independently -- a subagent
  // runs inside an already-bound interactive session, so a cached sessionId alone can't tell them apart.
  const beginSession = (ctx: { hasUI: boolean; cwd: string; sessionManager: { getSessionId(): string | undefined } }) => {
    if (!ctx.hasUI) return;
    const prev = sessionId;
    let id = "";
    try { id = ctx.sessionManager.getSessionId() ?? ""; } catch {}
    sessionId = id || `omp-${Date.now()}`;
    cwd = ctx.cwd;
    if (prev && prev !== sessionId) {
      post({ hook_event_name: "SessionEnd", session_id: prev }, 3_000).catch(() => {});
    }
    // SessionStart carries tap-to-open host context (process.env, read at bind time), mirroring the
    // Claude beacon-session shim: TERM_PROGRAM, WARP_FOCUS_URL (Warp per-pane handle), bundle id.
    post({ hook_event_name: "SessionStart",
           host_app: process.env.TERM_PROGRAM ?? "",
           focus_url: process.env.WARP_FOCUS_URL ?? "",
           bundle_id: process.env.__CFBundleIdentifier ?? "" }, 3_000).catch(() => {});
  };

  pi.on("session_start", async (_e, ctx) => beginSession(ctx));
  pi.on("session_switch", async (_e, ctx) => beginSession(ctx));
  pi.on("session_branch", async (_e, ctx) => beginSession(ctx));
  // agent_start (not `input`): `input` fires for slash commands like /settings and would
  // falsely mark the session working.
  pi.on("agent_start", async () => { void lifecycle("UserPromptSubmit"); });
  pi.on("agent_end", async (event) => {
    if ((event as { willContinue?: boolean }).willContinue) return; // auto-continuation: still working
    void lifecycle("Stop");
  });
  // omp waits <=2s for shutdown handlers; awaiting a bounded POST gets SessionEnd out before exit.
  pi.on("session_shutdown", async () => { await lifecycle("SessionEnd", 1_500); });

  // Approval MIRRORING, not gating (v4). omp resolves tool approval BEFORE extensions see `tool_call`,
  // `tool_approval_requested` is notify-only (the runner discards its return value), and the approval
  // prompt's UI context is mode-owned -- so an extension cannot answer omp's prompt the way the
  // Claude/Codex PermissionRequest hooks answer theirs. Blocking `tool_call` anyway would either
  // double-prompt (Mac first, then device) or gate exactly what `yolo` told omp not to ask about.
  // So Beacon mirrors the wait instead: the device shows that session as `question` and tap-to-open
  // focuses its terminal, and the user answers where omp is actually asking.
  pi.on("tool_approval_requested", async (_e, ctx) => {
    if (!ctx.hasUI) return;
    void lifecycle("Notification");
  });
  pi.on("tool_approval_resolved", async (_e, ctx) => {
    if (!ctx.hasUI) return;
    void lifecycle("ApprovalResolved");
  });
}
"""#
}
