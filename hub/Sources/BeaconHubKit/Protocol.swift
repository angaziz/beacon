import Foundation

// The FROZEN hub<->device protocol (tech.md §7.1/§7.2), Swift side. Mirrors the device records.h /
// hub_proto.cpp. Status frames (hub->device) and commands (device->hub) are newline-delimited JSON,
// every frame carrying "v":1. Encoders omit nil windows; the device treats an absent/null window as
// "unavailable" (pct -1, shown as "--").

public struct UsageWindow: Codable, Equatable {
    public var pct: Int?     // 0...100, or nil => unavailable (omitted from JSON / device reads -1)
    public var reset: Int    // Unix epoch seconds; 0 = unknown
    public init(pct: Int?, reset: Int) { self.pct = pct; self.reset = reset }
}

public struct ProviderUsage: Codable, Equatable {
    public var h5: UsageWindow
    public var d7: UsageWindow
    // true => the windows carry last-known-good held through a transient failure (device dims them).
    // MUST be nil (not false) on live: synthesized Codable encodes `false` but omits nil, and §A only
    // ever carries `"stale":true`. Additive v:1 ext (issue #108), mirrors qlen/loc.
    public var stale: Bool?
    public init(h5: UsageWindow, d7: UsageWindow, stale: Bool? = nil) {
        self.h5 = h5; self.d7 = d7; self.stale = stale
    }
    public static var unavailable: ProviderUsage {
        ProviderUsage(h5: UsageWindow(pct: nil, reset: 0), d7: UsageWindow(pct: nil, reset: 0))
    }
}

public struct Usage: Codable, Equatable {
    public var claude: ProviderUsage
    public var codex: ProviderUsage
    public init(claude: ProviderUsage, codex: ProviderUsage) { self.claude = claude; self.codex = codex }
}

public struct BuddyPrompt: Codable, Equatable {
    public var id: String
    public var tool: String
    public var hint: String
    public var qlen: Int?   // total pending prompts incl. this front one; nil/<=1 => lone prompt (omitted)
    public init(id: String, tool: String, hint: String, qlen: Int? = nil) {
        self.id = id; self.tool = tool; self.hint = hint; self.qlen = qlen
    }
}

public struct BuddyState: Codable, Equatable {
    public var running: Int
    public var waiting: Int
    public var tokens: Int
    public var contextPct: Int
    public var entries: [String]
    public var prompt: BuddyPrompt?   // nil => idle (absence of prompt, tech.md §7.1)
    public init(running: Int = 0, waiting: Int = 0, tokens: Int = 0, contextPct: Int = 0,
                entries: [String] = [], prompt: BuddyPrompt? = nil) {
        self.running = running; self.waiting = waiting; self.tokens = tokens
        self.contextPct = contextPct; self.entries = entries; self.prompt = prompt
    }
    enum CodingKeys: String, CodingKey {
        case running, waiting, tokens, entries, prompt
        case contextPct = "context_pct"
    }
}

// Device location block (issue #54). The hub sources lat/lon + place name from CoreLocation/CLGeocoder
// and tz from TimeZone.current; the device persists it (precedence hub > cached > IP). Sent ONLY in the
// (re)connect full frame and in a loc-only frame on meaningful change -- never on the 30s heartbeat.
public struct Loc: Codable, Equatable {
    public var lat: Double
    public var lon: Double
    public var tz: String
    public var name: String
    public init(lat: Double, lon: Double, tz: String, name: String) {
        self.lat = lat; self.lon = lon; self.tz = tz; self.name = name
    }
}

public enum SessionLimits { public static let maxCount = 5; public static let labelMaxChars = 28; public static let idMaxChars = 6 }

public enum SessionState: String, Codable, Equatable {
    case working, waiting, attention, idle
    case waitingQueued = "waiting_queued"
}

public struct Session: Codable, Equatable {
    public var id: String
    public var label: String
    public var state: SessionState
    public var ts: Int            // Unix epoch seconds of last update
    public init(id: String, label: String, state: SessionState, ts: Int) {
        self.id = id; self.label = label; self.state = state; self.ts = ts
    }
}

// Standalone hub->device frame (design §4). NOT embedded in `buddy`: the combined status frame
// (usage+buddy+loc) already nears HUB_FRAME_MAX; a separate frame keeps the budget independent and
// lets old firmware ignore it (it still reads the unchanged `buddy`/`entries` frame).
public struct SessionsFrame: Codable {
    public var sessions: [Session]
    public let v: Int
    // Defensive cap/truncation at the wire boundary even though SessionRegistry is the sole producer:
    // guarantees the frame can never exceed the frozen caps regardless of caller.
    public init(_ sessions: [Session]) {
        self.sessions = sessions.prefix(SessionLimits.maxCount).map {
            Session(id: String($0.id.prefix(SessionLimits.idMaxChars)),
                    label: String($0.label.prefix(SessionLimits.labelMaxChars)),
                    state: $0.state, ts: $0.ts)
        }
        self.v = 1
    }
    public func encoded() throws -> Data {
        let enc = JSONEncoder(); enc.outputFormatting = [.sortedKeys]
        var d = try enc.encode(self); d.append(0x0A); return d
    }
}

// One hub->device status frame. usage/buddy/loc are independently optional (send what changed; the
// device keeps an absent block's last values). encoded() emits the §7.1 wire form with "v":1 + a \n.
public struct StatusFrame: Codable {
    public var usage: Usage?
    public var buddy: BuddyState?
    public var loc: Loc?
    public let v: Int
    public init(usage: Usage? = nil, buddy: BuddyState? = nil, loc: Loc? = nil) {
        self.usage = usage; self.buddy = buddy; self.loc = loc; self.v = 1
    }

    public func encoded() throws -> Data {
        let enc = JSONEncoder()
        enc.outputFormatting = [.sortedKeys]   // deterministic for tests; device is key-order-agnostic
        var data = try enc.encode(self)
        data.append(0x0A)                       // newline-delimited framing
        return data
    }
}

// device->hub commands (tech.md §7.1). Parsed from a reassembled RX line.
public enum DeviceCommand: Equatable {
    case permission(id: String, approve: Bool)
    // One ack per completed config snapshot (issue #92). Echoes the pushed `rev`; on ok carries the
    // applied ticker count, on reject the first `err` (see TickerConfig / CONTRACT.md §C for the enum).
    case configAck(rev: UInt32, ok: Bool, count: Int?, err: String?)
    // Per-chunk snapshot of its running ticker list (issue #105). Per-chunk; the caller
    // reassembles. rev is always 0 (the device does not persist the hub's rev). Used so a fresh hub
    // can adopt the list the device already holds.
    case report(what: String, rev: UInt32, part: Int, parts: Int, rows: [TickerRow])
    // Tap-to-open: device asks hub to focus the terminal/editor for session `id` (issue #110, P2-b).
    case open(id: String)

    public static func parse(_ data: Data) -> DeviceCommand? {
        guard let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              (obj["v"] as? Int) == 1, let cmd = obj["cmd"] as? String else { return nil }
        switch cmd {
        case "permission":
            guard let id = obj["id"] as? String, let dec = obj["decision"] as? String else { return nil }
            return .permission(id: id, approve: dec == "approve")
        case "open":
            guard let id = obj["id"] as? String, !id.isEmpty else { return nil }
            return .open(id: id)
        case "config_ack":
            guard let rev = obj["rev"] as? Int, rev >= 0, let ok = obj["ok"] as? Bool else { return nil }
            return .configAck(rev: UInt32(rev), ok: ok, count: obj["count"] as? Int, err: obj["err"] as? String)
        case "report":
            guard (obj["what"] as? String) == "tickers",
                  let rev = obj["rev"] as? Int, rev >= 0,
                  let part = obj["part"] as? Int, let parts = obj["parts"] as? Int,
                  parts > 0, part >= 0, part < parts,
                  let arr = obj["tickers"] as? [[String: Any]] else { return nil }
            var rows = [TickerRow]()
            for r in arr {
                guard let id = r["id"] as? String, !id.isEmpty,
                      id.utf8.count <= TickerLimits.idMaxBytes,
                      let src = (r["src"] as? String).flatMap(TickerSource.init(rawValue:)),
                      let sym = r["sym"] as? String, sym.utf8.count <= TickerLimits.symMaxBytes,
                      let name = r["name"] as? String, name.utf8.count <= TickerLimits.nameMaxBytes,
                      let kind = (r["kind"] as? String).flatMap(TickerKind.init(rawValue:)),
                      let cadence = r["cadence"] as? Int, let stale = r["stale"] as? Int,
                      let basis = (r["basis"] as? String).flatMap(ChangeBasis.init(rawValue:))
                else { return nil }   // any malformed / over-cap row drops the whole chunk (parity with config)
                rows.append(TickerRow(id: id, src: src, sym: sym, name: name,
                                      kind: kind, cadence: cadence, stale: stale, basis: basis))
            }
            return .report(what: "tickers", rev: UInt32(rev), part: part, parts: parts, rows: rows)
        default:
            return nil
        }
    }
}

// hub -> Claude Code permission-hook HTTP response (NOT a BLE frame). PreToolUse and PermissionRequest
// require DIFFERENT decision shapes (CC v2.1.x): PreToolUse uses hookSpecificOutput.permissionDecision;
// PermissionRequest uses hookSpecificOutput.decision.behavior. Emit the one matching the originating
// event, else the device's approve/deny does not gate the tool. Beacon hooks PermissionRequest (fires
// only when permission is actually needed); PreToolUse is kept for back-compat.
public enum HookResponse {
    // `message` names the deny cause in the CC TUI (e.g. "Beacon device offline"); nil falls back to
    // the generic reason. Ignored on allow (CONTRACT.md §C.3: message only on deny).
    public static func permission(event: String, allow: Bool, message: String? = nil) -> Data {
        let denyReason = message ?? "Denied on Beacon device"
        let inner: [String: Any]
        switch event {
        case "PermissionRequest":
            var decision: [String: Any] = ["behavior": allow ? "allow" : "deny"]
            if !allow { decision["message"] = denyReason }   // message optional; allow needs none
            inner = ["hookEventName": "PermissionRequest", "decision": decision]
        default:   // PreToolUse (and aliases): permissionDecision allow|deny
            inner = ["hookEventName": event,
                     "permissionDecision": allow ? "allow" : "deny",
                     "permissionDecisionReason": allow ? "Approved on Beacon device" : denyReason]
        }
        let payload: [String: Any] = ["hookSpecificOutput": inner]
        return (try? JSONSerialization.data(withJSONObject: payload, options: [.sortedKeys])) ?? Data("{}".utf8)
    }

    // Do NOT gate -- defer to Claude Code's own interactive prompt. Used for AskUserQuestion, which is a
    // multi-option question the device can't (and shouldn't) answer; `allow` here could let an auto-accept
    // mode resolve it with no human pick, so we explicitly hand it to the Mac instead.
    public static func permissionAsk(event: String) -> Data {
        switch event {
        case "PermissionRequest":
            // PermissionRequest's decision.behavior accepts only allow/deny (no "ask", unlike PreToolUse);
            // an unsupported value fails to defer. Emit NO decision -- CC then falls through to its own
            // interactive prompt on the Mac, which is exactly the passthrough we want.
            return Data("{}".utf8)
        default:   // PreToolUse (and aliases): permissionDecision supports "ask" directly.
            let payload = ["hookSpecificOutput": ["hookEventName": event, "permissionDecision": "ask"]]
            return (try? JSONSerialization.data(withJSONObject: payload, options: [.sortedKeys])) ?? Data("{}".utf8)
        }
    }
}

// hub->device ack/err for a received command (tech.md §7.1).
public enum HubAck {
    public static func ack(id: String, ok: Bool) -> Data {
        frame(["v": 1, "ack": id, "ok": ok])
    }
    public static func err(id: String, reason: String) -> Data {
        frame(["v": 1, "err": reason, "id": id])
    }
    private static func frame(_ obj: [String: Any]) -> Data {
        var d = (try? JSONSerialization.data(withJSONObject: obj, options: [.sortedKeys])) ?? Data()
        d.append(0x0A)
        return d
    }
}
