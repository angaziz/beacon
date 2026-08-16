import Foundation

// Pure pi-extension logic (issue #149), split out of the executable so the managed extension source
// and its "is this the current managed file?" check are host-tested. The executable's HooksInstaller
// only does file IO (mkdir, back up an unrecognized file, atomic write) around this.
//
// Pi auto-discovers every module in ~/.pi/agent/extensions/, so installation is a single self-contained
// file. The file is wholly Beacon-managed: detection is content equality (modulo surrounding-whitespace
// trim), not a marker substring, so a truncated/edited/older-or-newer file reads as NOT current and
// Settings offers reinstall.
public enum PiHooks {

    public static let routePath = "/pi/hook"
    public static let extensionFileName = "beacon.ts"

    public static func isCurrent(_ content: String) -> Bool {
        content.trimmingCharacters(in: .whitespacesAndNewlines)
            == extensionSource.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private static let ppsConfigKeys: Set<String> = [
        "$schema", "debugLog", "permissionReviewLog", "yoloMode", "doublePressToConfirm",
        "toolInputPreviewMaxLength", "toolTextSummaryMaxLength", "piInfrastructureReadPaths",
        "authorizerChain", "permission", "shellTools"
    ]

    private static func ppsConfig(_ content: String) -> [String: Any]? {
        guard let config = try? JSONSerialization.jsonObject(with: Data(content.utf8)) as? [String: Any],
              Set(config.keys).isSubset(of: ppsConfigKeys),
              config["authorizerChain"].map({ $0 is [String] }) ?? true else { return nil }
        return config
    }

    public static func mergePpsAuthorizerChain(_ content: String) -> String? {
        guard let config = ppsConfig(content) else { return nil }
        let chain = config["authorizerChain"] as? [String] ?? []
        guard !chain.contains("beacon") else { return content }
        let bytes = Array(content.utf8)
        if let closingBracket = authorizerChainClosingBracket(in: bytes) {
            var merged = bytes
            merged.insert(contentsOf: chain.isEmpty ? Array("\"beacon\"".utf8) : Array(", \"beacon\"".utf8), at: closingBracket)
            return String(bytes: merged, encoding: .utf8)
        }
        guard let brace = bytes.firstIndex(of: 123) else { return nil }
        let hasProperties = bytes[(brace + 1)...].contains(where: { !isJSONWhitespace($0) })
        let indent = indentation(after: brace, in: bytes)
        var merged = bytes
        let entry = Array("\"authorizerChain\":[\"beacon\"]".utf8) + (hasProperties ? [44] : [])
        merged.insert(contentsOf: (hasProperties && !indent.isEmpty ? [10] + indent : []) + entry, at: brace + 1)
        return String(bytes: merged, encoding: .utf8)
    }

    public static func ppsAuthorizerChainContainsBeacon(_ content: String) -> Bool {
        (ppsConfig(content)?["authorizerChain"] as? [String])?.contains("beacon") == true
    }

    private static func isJSONWhitespace(_ byte: UInt8) -> Bool {
        byte == 32 || byte == 9 || byte == 10 || byte == 13
    }

    private static func indentation(after brace: Int, in bytes: [UInt8]) -> [UInt8] {
        guard let newline = bytes[(brace + 1)...].firstIndex(of: 10) else { return [] }
        return Array(bytes[(newline + 1)...].prefix { $0 == 32 || $0 == 9 })
    }

    // JSON is already validated above. This scanner only finds the original array's closing bracket so
    // the merge does not serialize, reorder, or otherwise alter user policy maps.
    private static func authorizerChainClosingBracket(in bytes: [UInt8]) -> Int? {
        var depth = 0, index = 0
        while index < bytes.count {
            if bytes[index] == 34 {
                let start = index
                index += 1
                while index < bytes.count { if bytes[index] == 92 { index += 2; continue }; if bytes[index] == 34 { break }; index += 1 }
                guard index < bytes.count else { return nil }
                if depth == 1, String(bytes: bytes[(start + 1)..<index], encoding: .utf8) == "authorizerChain" {
                    var value = index + 1
                    while value < bytes.count, isJSONWhitespace(bytes[value]) || bytes[value] == 58 { value += 1 }
                    guard value < bytes.count, bytes[value] == 91 else { return nil }
                    var arrayDepth = 1
                    value += 1
                    while value < bytes.count, arrayDepth > 0 {
                        if bytes[value] == 34 { value += 1; while value < bytes.count { if bytes[value] == 92 { value += 2; continue }; if bytes[value] == 34 { break }; value += 1 } }
                        else if bytes[value] == 91 { arrayDepth += 1 }
                        else if bytes[value] == 93 { arrayDepth -= 1 }
                        value += 1
                    }
                    return arrayDepth == 0 ? value - 1 : nil
                }
            } else if bytes[index] == 123 || bytes[index] == 91 { depth += 1 }
            else if bytes[index] == 125 || bytes[index] == 93 { depth -= 1 }
            index += 1
        }
        return nil
    }

    public static let extensionSource: String = #"""
// beacon-pi v3 -- managed by Beacon hub; do not edit (reinstall overwrites).
import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { dirname, relative, resolve } from "node:path";
import { lstat, readlink, realpath } from "node:fs/promises";

const HUB = "http://127.0.0.1:8765/pi/hook";
const PERMISSION = "http://127.0.0.1:8765/pi/permission";
const CIRCUIT_MS = 60_000;
let circuitUntil = 0;

export default function beacon(pi: ExtensionAPI) {
  let sessionId = "";
  let cwd = "";

  const post = (body: Record<string, unknown>, timeoutMs: number) =>
    fetch(HUB, { method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ session_id: sessionId, cwd, ...body }), signal: AbortSignal.timeout(timeoutMs) });
  const lifecycle = (event: string, timeoutMs = 3_000) =>
    !sessionId ? Promise.resolve() : post({ hook_event_name: event }, timeoutMs).catch(() => {});

  const beginSession = (ctx: { hasUI: boolean; cwd: string; sessionManager: { getSessionFile(): string | undefined } }) => {
    if (!ctx.hasUI) return;
    const previous = sessionId;
    let file = "";
    try { file = ctx.sessionManager.getSessionFile() ?? ""; } catch {}
    sessionId = file.split(/[\\/]/).pop() || `pi-${Date.now()}`;
    cwd = ctx.cwd;
    if (previous && previous !== sessionId) void post({ hook_event_name: "SessionEnd", session_id: previous }, 3_000).catch(() => {});
    void post({ hook_event_name: "SessionStart", host_app: process.env.TERM_PROGRAM ?? "",
      focus_url: process.env.WARP_FOCUS_URL ?? "", bundle_id: process.env.__CFBundleIdentifier ?? "" }, 3_000).catch(() => {});
  };

  type PpsService = { registerAuthorizer: (name: string, fn: (details: Record<string, unknown>) => Promise<{ kind: "allow" | "deny" | "defer" }>) => unknown };
  const registered = new WeakSet<object>();
  // The permission system's envelope treats absent and unrecognized surfaces as excluded.
  const delegableSurfaces = new Set(["bash", "read", "write", "edit", "find", "grep", "ls", "mcp", "skill"]);
  let authorizerCtx: { hasUI: boolean; cwd: string; signal?: AbortSignal;
    ui: { select: (title: string, options: string[], opts: { signal: AbortSignal }) => Promise<string | undefined> } } | undefined;
  const ppsService = async (): Promise<PpsService | undefined> => {
    const published = (globalThis as Record<symbol, PpsService | undefined>)[Symbol.for("@gotgenes/pi-permission-system:service")];
    if (published) return published;
    try {
      const mod = await import("@gotgenes/pi-permission-system") as { getPermissionsService?: () => PpsService | undefined };
      return mod.getPermissionsService?.();
    } catch { return undefined; }
  };
  const ppsActive = async () => {
    if (await ppsService()) return true;
    try { await import("@gotgenes/pi-permission-system"); return true; } catch { return false; }
  };

  const canonical = async (path: string): Promise<string> => {
    let probe = resolve(path);
    const tail: string[] = [];
    for (;;) {
      try { return resolve(await realpath(probe), ...tail.reverse()); }
      catch {
        const parent = dirname(probe);
        if (parent === probe) return resolve(path);
        try {
          const stat = await lstat(probe);
          const existing = stat.isSymbolicLink()
            ? await canonical(resolve(parent, await readlink(probe)))
            : await realpath(probe);
          return resolve(existing, ...tail.reverse());
        } catch {}
        tail.push(probe.slice(parent.length).replace(/^[\\/]+/, ""));
        probe = parent;
      }
    }
  };
  const outsideCwd = async (file: unknown, base: string) => {
    if (typeof file !== "string") return false;
    const [target, root] = await Promise.all([canonical(resolve(base, file)), canonical(base)]);
    const rel = relative(root, target);
    return rel === ".." || rel.startsWith(`..${process.platform === "win32" ? "\\" : "/"}`) || resolve(root, rel) !== target;
  };
  const asks = async (event: { toolName: string; input: Record<string, unknown> }, base: string) =>
    event.toolName === "bash" || ((event.toolName === "write" || event.toolName === "edit") && await outsideCwd(event.input.path ?? event.input.file_path, base));

  const linked = (signal?: AbortSignal) => {
    const controller = new AbortController();
    const abort = () => controller.abort();
    if (signal?.aborted) abort();
    else signal?.addEventListener("abort", abort, { once: true });
    return { controller, dispose: () => signal?.removeEventListener("abort", abort) };
  };
  const select = (ctx: { ui: { select: (title: string, options: string[], opts: { signal: AbortSignal }) => Promise<string | undefined> } }, signal: AbortSignal) =>
    ctx.ui.select("Beacon permission", ["Allow", "Deny"], { signal });
  const fallback = async (ctx: { signal?: AbortSignal; ui: { select: (title: string, options: string[], opts: { signal: AbortSignal }) => Promise<string | undefined> } }) => {
    const link = linked(ctx.signal);
    try {
      const choice = await select(ctx, link.controller.signal).catch(() => undefined);
      return choice === "Deny" ? false : choice === "Allow" ? true : undefined;
    } finally { link.dispose(); }
  };
  const commit = (id: string, applied: boolean) =>
    fetch(PERMISSION, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ id, applied }), signal: AbortSignal.timeout(1_500) }).catch(() => {});
  const preflight = async (signal?: AbortSignal) => {
    const link = linked(signal);
    const timeout = setTimeout(() => link.controller.abort(), 400);
    try {
      const response = await fetch(PERMISSION, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ probe: true }), signal: link.controller.signal });
      if (!response.ok) return undefined;
      const body = await response.json() as { device?: boolean };
      return body.device === true ? true : body.device === false ? false : undefined;
    } catch { return undefined; } finally { clearTimeout(timeout); link.dispose(); }
  };

  const choose = async (event: { toolCallId: string; toolName: string; input: Record<string, unknown> }, ctx: {
    hasUI: boolean; cwd: string; signal?: AbortSignal; ui: { select: (title: string, options: string[], opts: { signal: AbortSignal }) => Promise<string | undefined> };
  }, fromPps = false) => {
    if (!ctx.hasUI || (!fromPps && (!await asks(event, ctx.cwd) || await ppsActive()))) return undefined;
    if (Date.now() < circuitUntil) return fromPps ? undefined : fallback(ctx);
    const reachable = await preflight(ctx.signal);
    if (reachable === undefined) { circuitUntil = Date.now() + CIRCUIT_MS; return fromPps ? undefined : fallback(ctx); }
    circuitUntil = 0;
    if (!reachable) return fromPps ? undefined : fallback(ctx);
    const tui = linked(ctx.signal);
    const hub = linked(ctx.signal);
    const noChoice = Symbol("pending");
    let localChoice: string | undefined | typeof noChoice = noChoice;
    const local = select(ctx, tui.controller.signal).then(choice => { localChoice = choice; return { local: choice }; })
      .catch(() => { localChoice = undefined; return { local: undefined }; });
    const remote = fetch(PERMISSION, { method: "POST", headers: { "content-type": "application/json" }, signal: hub.controller.signal,
      body: JSON.stringify({ session_id: sessionId, tool_name: event.toolName, tool_input: event.input, tool_call_id: event.toolCallId })
    }).then(async response => {
      if (!response.ok) return { unavailable: true };
      try { return await response.json() as unknown; } catch { return { unavailable: true }; }
    }).catch(() => ({ unavailable: true }));
    try {
      let winner = await Promise.race([local, remote.then(value => ({ remote: value }))]);
      // Cancelling the selector is abstention, not allow: leave the device request live.
      if ("local" in winner && winner.local === undefined && !ctx.signal?.aborted) winner = { remote: await remote };
      if ("local" in winner) {
        hub.controller.abort();
        return winner.local === "Deny" ? false : winner.local === "Allow" ? true : undefined;
      }
      const payload = winner.remote as { device?: boolean; id?: string; approve?: boolean; unavailable?: boolean } | undefined;
      if (!payload || payload.device === false || payload.unavailable || typeof payload.id !== "string" || typeof payload.approve !== "boolean") {
        if (fromPps) { tui.controller.abort(); return undefined; }
        return (await local).local === "Deny" ? false : (await local).local === "Allow" ? true : undefined;
      }
      await new Promise(resolve => setTimeout(resolve, 0));
      if (localChoice !== noChoice && localChoice !== undefined) {
        tui.controller.abort();
        await commit(payload.id, false);
        return localChoice === "Deny" ? false : true;
      }
      tui.controller.abort();
      await commit(payload.id, true);
      return payload.approve;
    } finally { tui.controller.abort(); hub.controller.abort(); tui.dispose(); hub.dispose(); }
  };

  const registerPps = async () => {
    const service = await ppsService();
    if (!service || registered.has(service as object)) return;
    try {
      service.registerAuthorizer("beacon", async details => {
        const surface = (details.accessIntent as { surface?: string } | undefined)?.surface ?? details.surface;
        if (typeof surface !== "string" || !delegableSurfaces.has(surface) || !authorizerCtx?.hasUI) return { kind: "defer" };
        try {
          const decision = await choose({ toolCallId: String(details.toolCallId ?? details.requestId ?? "pps"),
            toolName: String(details.toolName ?? surface ?? "pps"),
            input: { path: details.path, command: details.command, target: details.target } }, authorizerCtx, true);
          return decision === true ? { kind: "allow" } : decision === false ? { kind: "deny" } : { kind: "defer" };
        } catch { return { kind: "defer" }; }
      });
      registered.add(service as object);
    } catch {}
  };
  pi.events?.on("permissions:ready", () => { void registerPps(); });
  pi.on("tool_call", async (event, ctx) => {
    authorizerCtx = ctx;
    try {
      if (await ppsActive()) { void registerPps(); return {}; }
      const decision = await choose(event as { toolCallId: string; toolName: string; input: Record<string, unknown> }, ctx);
      return decision === false ? { block: true, reason: "Denied on Beacon" } : {};
    } catch { return {}; }
  });
  pi.on("session_start", async (_event, ctx) => { authorizerCtx = ctx; beginSession(ctx); void registerPps(); });
  pi.on("agent_start", async (_event, ctx) => {
    if (!ctx.hasUI) return;
    void lifecycle("UserPromptSubmit");
  });
  pi.on("agent_settled", async (_event, ctx) => {
    if (!ctx.hasUI) return;
    void lifecycle("Stop");
  });
  pi.on("session_shutdown", async (_event, ctx) => {
    if (!ctx.hasUI) return;
    await lifecycle("SessionEnd", 1_500);
  });
}
"""#
}
