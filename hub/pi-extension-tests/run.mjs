import assert from "node:assert/strict";
import { cp, mkdtemp, mkdir, readFile, rm, symlink, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

const swift = await readFile(new URL("../Sources/BeaconHubKit/PiHooks.swift", import.meta.url), "utf8");
const source = swift.match(/extensionSource: String = #"""\n([\s\S]*?)\n"""#/)?.[1];
assert(source, "could not extract PiHooks.extensionSource");
const sandbox = await mkdtemp(join(tmpdir(), "beacon-pi-test-"));
const extension = join(sandbox, "beacon.ts");
await writeFile(extension, source);

let serial = 0;
async function load(fetchImpl, ppsService) {
  globalThis.fetch = fetchImpl;
  delete globalThis[Symbol.for("@gotgenes/pi-permission-system:service")];
  if (ppsService) globalThis[Symbol.for("@gotgenes/pi-permission-system:service")] = ppsService;
  const mod = await import(`file://${extension}?case=${serial++}`);
  const handlers = new Map();
  const events = new Map();
  await mod.default({ on: (event, handler) => handlers.set(event, handler), events: { on: (event, handler) => events.set(event, handler) } });
  assert(handlers.has("tool_call"), "tool gate not registered");
  return { handlers, events };
}
async function handlersFor(fetchImpl, ppsService) { return (await load(fetchImpl, ppsService)).handlers; }
function json(value) { return new Response(JSON.stringify(value), { status: 200 }); }
function context(cwd, select, signal) { return { hasUI: true, cwd, signal, ui: { select } }; }
async function call(handlers, event, ctx) { return handlers.get("tool_call")(event, ctx); }
const event = (toolName, input = {}) => ({ toolName, input, toolCallId: `call-${serial}` });
const localAllow = async () => "Allow";
const localDeny = async () => "Deny";
const noDevice = async () => json({ device: false });

for (const required of ["beacon-pi v3", "/pi/permission", "ctx.ui.select", "AbortController", "ppsActive", "registerAuthorizer", "permissions:ready", "CIRCUIT_MS", "realpath", "commit(payload.id, false)"]) {
  assert(source.includes(required), `missing ${required}`);
}
console.log("load-clean: source contract markers present");

const root = join(sandbox, "project");
const sibling = join(sandbox, "project-old");
await mkdir(join(root, "inside"), { recursive: true });
await mkdir(sibling, { recursive: true });
await symlink(sibling, join(root, "escape"));
await symlink(join(sandbox, "outside-missing"), join(root, "dangling"));
const policyCases = [
  ["bash asks", event("bash"), true],
  ["inside write allows", event("write", { path: "inside/a" }), false],
  ["dot-dot escape asks", event("write", { path: "../outside/a" }), true],
  ["sibling-prefix asks", event("edit", { path: sibling + "/a" }), true],
  ["symlink escape asks", event("write", { path: "escape/a" }), true],
  ["dangling symlink escape asks", event("write", { path: "dangling/a" }), true],
  ["missing target outside asks", event("edit", { path: "../missing/a" }), true]
];
for (const [name, tool, expectsAsk] of policyCases) {
  let calls = 0;
  const handlers = await handlersFor(async () => { calls++; return json({ device: false }); });
  const result = await call(handlers, tool, context(root, localAllow));
  assert.equal(result.block, undefined, name);
  assert.equal(calls > 0, expectsAsk, name);
}
console.log(`policy matrix: ${policyCases.length} cases passed`);

for (const [name, fetchImpl] of [
  ["hub down", async () => { throw new Error("ECONNREFUSED"); }],
  ["malformed JSON", async () => new Response("not-json", { status: 200 })],
  ["device absent", noDevice],
  ["unavailable", async () => json({ unavailable: true })]
]) {
  const handlers = await handlersFor(fetchImpl);
  const result = await call(handlers, event("bash"), context(root, localDeny));
  assert.equal(result.block, true, `${name} must fall back to local deny`);
}
console.log("failure ladder: down, malformed, device:false, unavailable pass to TUI");

let commits = [];
let tuiAborted = false;
const handlers = await handlersFor(async (_url, options) => {
  const body = JSON.parse(options.body);
  if (body.probe) return json({ device: true });
  if ("applied" in body) { commits.push(body); return json({ ok: true }); }
  return json({ id: "p-device", approve: false });
});
const deviceResult = await call(handlers, event("bash"), context(root, (_title, _options, opts) => new Promise(resolve => {
  opts.signal.addEventListener("abort", () => { tuiAborted = true; resolve(undefined); }, { once: true });
})));
assert.equal(deviceResult.block, true, "device deny blocks");
assert.deepEqual(commits, [{ id: "p-device", applied: true }]);
assert(tuiAborted, "device win dismisses TUI");
console.log("device wins: commit applied:true and TUI dismissed");

commits = [];
let heldAborted = false;
const tuiFirst = await handlersFor(async (_url, options) => {
  const body = JSON.parse(options.body);
  if (body.probe) return json({ device: true });
  if ("applied" in body) { commits.push(body); return json({ ok: true }); }
  return new Promise((_, reject) => options.signal.addEventListener("abort", () => { heldAborted = true; reject(new Error("withdrawn")); }, { once: true }));
});
const tuiResult = await call(tuiFirst, event("bash"), context(root, localDeny));
assert.equal(tuiResult.block, true, "TUI deny blocks");
assert(heldAborted, "TUI win withdraws held hub request");
assert.deepEqual(commits, [], "pre-delivery TUI win has no device ack to commit");
console.log("TUI wins before delivery: held request withdrawn, no commit");

commits = [];
let releaseLocal;
const crossing = await handlersFor(async (_url, options) => {
  const body = JSON.parse(options.body);
  if (body.probe) return json({ device: true });
  if ("applied" in body) { commits.push(body); return json({ ok: true }); }
  return new Promise(resolve => {
    resolve(json({ id: "p-cross", approve: true }));
    setTimeout(() => releaseLocal("Deny"), 0);
  });
});
const crossResult = await call(crossing, event("bash"), context(root, () => new Promise(resolve => { releaseLocal = resolve; })));
assert.equal(crossResult.block, true, "crossing local deny wins");
assert.deepEqual(commits, [{ id: "p-cross", applied: false }]);
console.log("crossing race: local verdict commits applied:false");

let requests = 0;
const slowHealthy = await handlersFor(async (_url, options) => {
  const body = JSON.parse(options.body); requests++;
  if (body.probe) return json({ device: true });
  return new Promise(resolve => setTimeout(() => resolve(json({ id: "slow", approve: true })), 450));
});
await call(slowHealthy, event("bash"), context(root, () => new Promise(resolve => setTimeout(() => resolve("Allow"), 500))));
await call(slowHealthy, event("bash"), context(root, localAllow));
assert(requests >= 4, "healthy slow long-poll does not open circuit");
console.log("healthy slow long-poll: >400ms decision does not open circuit");

let probeCalls = 0;
const breaker = await handlersFor(async (_url, options) => {
  const body = JSON.parse(options.body); probeCalls++;
  if (body.probe && probeCalls === 1) return new Promise((_, reject) => options.signal.addEventListener("abort", () => reject(new Error("timeout")), { once: true }));
  if (body.probe) return json({ device: false });
  throw new Error("held request should not happen");
});
await call(breaker, event("bash"), context(root, localAllow));
const afterOpen = probeCalls;
await call(breaker, event("bash"), context(root, localAllow));
assert.equal(probeCalls, afterOpen, "open circuit skips probe");
// Reload resets module circuit state, representing expiry followed by a successful preflight.
const closed = await handlersFor(async (_url, options) => { assert(JSON.parse(options.body).probe); return json({ device: false }); });
await call(closed, event("bash"), context(root, localAllow));
console.log("circuit close: successful preflight resumes hub eligibility");

let abortTui = false;
const aborter = new AbortController();
const abortHandlers = await handlersFor(async (_url, options) => {
  const body = JSON.parse(options.body);
  if (body.probe) return json({ device: true });
  return new Promise((_, reject) => options.signal.addEventListener("abort", () => reject(new Error("aborted")), { once: true }));
});
const abortCall = call(abortHandlers, event("bash"), context(root, (_t, _o, opts) => new Promise(resolve => opts.signal.addEventListener("abort", () => { abortTui = true; resolve(undefined); }, { once: true })), aborter.signal));
setTimeout(() => aborter.abort(), 10);
assert.deepEqual(await abortCall, {}, "ctx abort returns no verdict");
assert(abortTui, "ctx abort dismisses local selector");
console.log("ctx.signal abort: both surfaces dismissed without hang");

const headless = await handlersFor(async () => { throw new Error("headless must not gate"); });
assert.deepEqual(await call(headless, event("bash"), { hasUI: false, cwd: root, ui: { select: localDeny } }), {});
console.log("headless: no gate");

const failedCommit = await handlersFor(async (_url, options) => {
  const body = JSON.parse(options.body);
  if (body.probe) return json({ device: true });
  if ("applied" in body) throw new Error("commit down");
  return json({ id: "commit-fail", approve: true });
});
assert.deepEqual(await call(failedCommit, event("bash"), context(root, () => new Promise(() => {}))), {});
console.log("commit POST failure: bounded without unhandled rejection");
const ppsHandlers = await handlersFor(async () => { throw new Error("must not contact hub"); });
globalThis[Symbol.for("@gotgenes/pi-permission-system:service")] = {};
const ppsResult = await call(ppsHandlers, event("bash"), context(root, localDeny));
assert.equal(ppsResult.block, undefined, "pps presence makes standalone gate inert");
delete globalThis[Symbol.for("@gotgenes/pi-permission-system:service")];
console.log("pps step-down: standalone gate inert when service is present");

const tick = () => new Promise(resolve => setTimeout(resolve, 0));
async function ppsHarness(fetchImpl, select = localAllow) {
  let authorizer;
  let registrations = 0;
  const service = { registerAuthorizer(name, fn) { assert.equal(name, "beacon"); registrations++; authorizer = fn; return () => {}; } };
  const loaded = await load(fetchImpl, service);
  const ctx = { ...context(root, select), sessionManager: { getSessionFile: () => "pps.jsonl" } };
  await loaded.handlers.get("session_start")({}, ctx);
  await loaded.handlers.get("tool_call")(event("bash"), ctx);
  await tick();
  assert(authorizer, "pps authorizer registered after service publication");
  return { authorizer, registrations, loaded, ctx };
}
const ppsDetails = (surface = "bash") => ({ requestId: `pps-${serial++}`, toolCallId: `pps-call-${serial}`, toolName: "bash", surface, accessIntent: { surface }, command: "git push" });

for (const [name, approve, expected] of [["bash allow", true, "allow"], ["device deny", false, "deny"]]) {
  const pps = await ppsHarness(async (_url, options) => {
    const body = JSON.parse(options.body);
    if (body.probe) return json({ device: true });
    if ("applied" in body) return json({ ok: true });
    return json({ id: `pps-${name}`, approve });
  }, () => new Promise(() => {}));
  assert.equal((await pps.authorizer(ppsDetails())).kind, expected);
  console.log(`pps authorizer: ${name} => ${expected}`);
}

let downDialogs = 0;
const ppsDown = await ppsHarness(async () => { throw new Error("hub down"); }, () => { downDialogs++; return Promise.resolve("Allow"); });
assert.equal((await ppsDown.authorizer(ppsDetails())).kind, "defer");
assert.equal(downDialogs, 0, "hub-down authorizer must defer without a Beacon dialog");
console.log("pps authorizer: hub down => defer");

const cancelled = new AbortController();
cancelled.abort();
const ppsCancel = await ppsHarness(async () => { throw new Error("cancel must not contact hub"); }, localAllow);
ppsCancel.ctx.signal = cancelled.signal;
assert.equal((await ppsCancel.authorizer(ppsDetails())).kind, "defer");
console.log("pps authorizer: ctx cancel => defer");

for (const surface of ["path", "external_directory", undefined]) {
  let network = 0;
  let dialogs = 0;
  const excluded = await ppsHarness(async () => { network++; return json({ device: true }); }, () => { dialogs++; return Promise.resolve("Allow"); });
  network = 0; // Ignore the session lifecycle mirror POST; the authorizer itself must be inert.
  const details = surface === undefined
    ? { requestId: `pps-${serial++}`, toolCallId: `pps-call-${serial}`, toolName: "bash", command: "git push" }
    : ppsDetails(surface);
  assert.equal((await excluded.authorizer(details)).kind, "defer");
  assert.equal(network, 0, `${surface ?? "missing surface"} must not contact the hub`);
  assert.equal(dialogs, 0, `${surface ?? "missing surface"} must not show a Beacon dialog`);
  console.log(`pps authorizer: ${surface ?? "missing surface"} excluded => immediate defer`);
}

const moduleDir = join(sandbox, "node_modules", "@gotgenes", "pi-permission-system");
await mkdir(moduleDir, { recursive: true });
await writeFile(join(moduleDir, "package.json"), '{"name":"@gotgenes/pi-permission-system","type":"module","exports":"./index.js"}');
await writeFile(join(moduleDir, "index.js"), "export const getPermissionsService = () => undefined;");
let unavailableCalls = 0;
const unavailable = await load(async () => { unavailableCalls++; return json({ device: false }); });
const unavailableResult = await unavailable.handlers.get("tool_call")(event("bash"), context(root, localDeny));
assert.deepEqual(unavailableResult, {}, "importable but unpublished pps keeps standalone gate inert");
assert.equal(unavailableCalls, 0, "unpublished pps must not contact hub");
console.log("pps authorizer: present-unavailable => standalone inert, authorizer absent");

const reload = await ppsHarness(async () => json({ device: false }));
await reload.loaded.handlers.get("session_start")({}, reload.ctx);
await tick();
assert.equal(reload.registrations, 1, "same live service registers exactly once across session reload");
console.log("pps authorizer: reload => one active registration");

const late = await load(async () => json({ device: false }));
let lateRegistrations = 0;
const lateService = { registerAuthorizer(name) { assert.equal(name, "beacon"); lateRegistrations++; } };
globalThis[Symbol.for("@gotgenes/pi-permission-system:service")] = lateService;
await late.events.get("permissions:ready")();
await tick();
assert.equal(lateRegistrations, 1, "late publication registers after permissions:ready");
console.log("pps authorizer: late publish => one registration");

let replacementRegistrations = 0;
const replacement = { registerAuthorizer(name) { assert.equal(name, "beacon"); replacementRegistrations++; } };
globalThis[Symbol.for("@gotgenes/pi-permission-system:service")] = replacement;
await late.events.get("permissions:ready")();
await tick();
assert.equal(replacementRegistrations, 1, "replacement service registers once");
assert.equal(lateRegistrations, 1, "old service is not registered again");
console.log("pps authorizer: service replacement => one registration per identity");

delete globalThis[Symbol.for("@gotgenes/pi-permission-system:service")];
await rm(sandbox, { recursive: true, force: true });
console.log("pi extension harness: all matrices passed");
