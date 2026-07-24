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
    // /omp/hook: request {hook_event_name, session_id, cwd, tool_name, tool_input}; response is the
    // HookResponse.permission shape ({} = passthrough). Timing chain (all under omp's 30s tool_call
    // handler ceiling, which itself blocks the tool on timeout): device 25s < hub 26s cap < fetch
    // abort 28s. Every transport/protocol failure fails closed (docs/tech.md §1).
    public static let extensionSource: String = #"""
// beacon-omp v1 -- managed by Beacon hub; do not edit (reinstall overwrites).
import type { ExtensionAPI } from "@oh-my-pi/pi-coding-agent";

const HUB = "http://127.0.0.1:8765/omp/hook";
const GATED_TOOLS = new Set(["bash"]);

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
  // activity to a dead id. hasUI=false covers print mode AND task subagents: buddy gates
  // interactive sessions only (empty sessionId disables everything).
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
    void lifecycle("SessionStart");
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

  pi.on("tool_call", async (event) => {
    if (!sessionId || !GATED_TOOLS.has(event.toolName)) return;
    const deny = (reason: string) => ({ block: true, reason });
    let res: Response;
    try {
      // 28s abort > hub 26s fail-closed cap > device 25s prompt timeout, all under omp's
      // 30s tool_call handler ceiling (which itself blocks on timeout).
      res = await post({
        hook_event_name: "PermissionRequest",
        tool_name: event.toolName,
        tool_input: event.input,
      }, 28_000);
    } catch {
      return deny("Beacon hub unreachable"); // fail closed (docs/tech.md §1)
    }
    if (!res.ok) return deny(`Beacon hub error (HTTP ${res.status})`);
    try {
      const json = await res.json() as { hookSpecificOutput?: { decision?: { behavior?: string; message?: string } } };
      const decision = json?.hookSpecificOutput?.decision;
      if (!decision) return; // {} = hub's explicit buddy-off/ask passthrough
      if (decision.behavior === "allow") return;
      return deny(decision.message ?? "Denied on Beacon device"); // deny AND any unknown behavior
    } catch {
      return deny("Beacon hub returned an unreadable decision");
    }
  });
}
"""#
}
