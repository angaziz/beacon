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
    public init(h5: UsageWindow, d7: UsageWindow) { self.h5 = h5; self.d7 = d7 }
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
    public init(id: String, tool: String, hint: String) { self.id = id; self.tool = tool; self.hint = hint }
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

// One hub->device status frame. usage/buddy are independently optional (send what changed; the device
// keeps an absent block's last values). encoded() emits the §7.1 wire form with "v":1 + a trailing \n.
public struct StatusFrame: Codable {
    public var usage: Usage?
    public var buddy: BuddyState?
    public let v: Int
    public init(usage: Usage? = nil, buddy: BuddyState? = nil) { self.usage = usage; self.buddy = buddy; self.v = 1 }

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
    case launch(text: String)

    public static func parse(_ data: Data) -> DeviceCommand? {
        guard let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              (obj["v"] as? Int) == 1, let cmd = obj["cmd"] as? String else { return nil }
        switch cmd {
        case "permission":
            guard let id = obj["id"] as? String, let dec = obj["decision"] as? String else { return nil }
            return .permission(id: id, approve: dec == "approve")
        case "launch":
            guard let text = obj["text"] as? String else { return nil }
            return .launch(text: text)
        default:
            return nil
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
