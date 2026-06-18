import Foundation

// Canonical ticker contract layer (issue #92, design 2026-06-17 §2). Mirrors the firmware parser in
// firmware/src/core/hub_proto.cpp (hub_parse_config_chunk / hub_config_accum_step). The hub is the
// source of truth for the desired list; it pushes a chunked full-snapshot `config` frame and the device
// acks via DeviceCommand.configAck. Wire strings are frozen -- see hub/CONTRACT.md.

public enum TickerSource: String, Codable { case binance, yahoo }
public enum TickerKind: String, Codable { case fx, crypto, index, etf }
public enum ChangeBasis: String, Codable { case prevClose = "prev_close", h24 = "24h" }

// One canonical ticker. JSON keys are frozen by the firmware row parser: id/src/sym/name/kind/cadence/
// stale/basis. Property names already match every key, so no CodingKeys remap is needed; the enum-typed
// fields serialize via their String rawValues (which equal the wire strings above).
public struct TickerRow: Codable, Equatable {
    public var id: String          // <= 15 chars, stable, derived from (src, sym)
    public var src: TickerSource
    public var sym: String         // wire-ready: Yahoo percent-encoded once, Binance raw
    public var name: String
    public var kind: TickerKind
    public var cadence: Int
    public var stale: Int
    public var basis: ChangeBasis

    public init(id: String, src: TickerSource, sym: String, name: String,
                kind: TickerKind, cadence: Int, stale: Int, basis: ChangeBasis) {
        self.id = id; self.src = src; self.sym = sym; self.name = name
        self.kind = kind; self.cadence = cadence; self.stale = stale; self.basis = basis
    }
}

public enum TickerID {
    // Deterministic id from (src, sym), invariant under list reorder/removal (design §4.3 Codex #9).
    // Form: "<srcPrefix><base32(fnv1a64(src:sym))>" -- 1-char source prefix + up to 13 base32 chars of a
    // 64-bit FNV-1a hash. Max length is 14 (< firmware FIN_ID_LEN-1 = 15). Same input => same id; a
    // collision needs a 64-bit hash clash, which is negligible for a <=16-row list.
    public static func make(src: TickerSource, sym: String) -> String {
        let prefix = src == .binance ? "b" : "y"
        let h = fnv1a64("\(src.rawValue):\(sym)")
        return prefix + base32(h)
    }

    private static func fnv1a64(_ s: String) -> UInt64 {
        var hash: UInt64 = 0xcbf2_9ce4_8422_2325          // FNV offset basis (64-bit)
        let prime: UInt64 = 0x0000_0100_0000_01b3          // FNV prime (64-bit)
        for byte in s.utf8 {
            hash ^= UInt64(byte)
            hash = hash &* prime
        }
        return hash
    }

    // Lowercase Crockford-ish base32 (0-9a-v) of a 64-bit value: ceil(64/5)=13 chars, no padding.
    private static func base32(_ value: UInt64) -> String {
        let alphabet = Array("0123456789abcdefghijklmnopqrstuv")
        if value == 0 { return "0" }
        var v = value
        var out = [Character]()
        while v > 0 {
            out.append(alphabet[Int(v & 0x1f)])
            v >>= 5
        }
        return String(out.reversed())
    }
}

public enum SymbolEncoding {
    // Percent-encode a RAW Yahoo symbol for direct interpolation into the URL path segment
    // /v8/finance/chart/<sym> (firmware finance.cpp interpolates with no escaping). Encode EXACTLY ONCE:
    // call only on raw symbols, never on already-encoded output (no idempotency guard -- the adapter owns
    // the single call site, design §4.3 Codex #10).
    //
    // Allowed (left literal) = the URL "unreserved" set plus the sub-delims Yahoo actually uses in its
    // symbols: '=' (e.g. EURUSD=X, GC=F) and '.'/'-' . '^' is NOT in that set, so it becomes %5E
    // (^GSPC => %5EGSPC). '/' (not present in any Yahoo symbol) would also be escaped, which is correct.
    //
    // Binance symbols are raw (BTCUSDT) and must NOT pass through here -- that is the B2 adapter's job.
    public static func yahooPath(_ rawSymbol: String) -> String {
        rawSymbol.addingPercentEncoding(withAllowedCharacters: allowed) ?? rawSymbol
    }

    private static let allowed: CharacterSet = {
        var set = CharacterSet.alphanumerics
        set.insert(charactersIn: "-._=")   // '=' is the FX/futures suffix marker; '^' deliberately excluded
        return set
    }()
}

public enum ConfigFrameError: Error, Equatable {
    case rowTooLarge(index: Int, bytes: Int, maxBytes: Int)
}

public enum ConfigFrame {
    // Pack rows into ordered, newline-terminated chunk frames, each <= maxBytes, never splitting a row.
    // `part` is 0-based, `parts` is the total chunk count; rows concatenated across chunks in order ==
    // input order (full-snapshot replace, design §2). Matches hub_parse_config_chunk byte-for-byte.
    //
    // Empty `rows` returns [] -- the firmware rejects an empty assembled snapshot (`empty`), so the hub
    // simply never pushes an empty list (the caller skips a [] result). Throws rowTooLarge if a single
    // row alone cannot fit under maxBytes (cannot be chunked away).
    public static func chunks(rows: [TickerRow], rev: UInt32, maxBytes: Int = 900) throws -> [Data] {
        if rows.isEmpty { return [] }

        // Greedily group rows into chunks small enough to serialize <= maxBytes. We compute parts first
        // (it must be embedded in every frame), so grouping ignores `parts` and re-measures with the
        // final parts value below; the per-row size dominates and the parts digits are bounded, so a
        // group that fits during planning still fits with the real header (verified by the final encode).
        let groups = try groupRows(rows, rev: rev, maxBytes: maxBytes)
        let parts = groups.count

        var frames = [Data]()
        for (idx, group) in groups.enumerated() {
            frames.append(try encode(rows: group, rev: rev, part: idx, parts: parts, maxBytes: maxBytes))
        }
        return frames
    }

    private static func groupRows(_ rows: [TickerRow], rev: UInt32, maxBytes: Int) throws -> [[TickerRow]] {
        var groups = [[TickerRow]]()
        var current = [TickerRow]()
        for (i, row) in rows.enumerated() {
            // Single-row floor: a row that can't fit even alone (with a worst-case header) is unchunkable.
            let solo = try encode(rows: [row], rev: rev, part: 0, parts: max(rows.count, 1),
                                  maxBytes: maxBytes, checkFit: false)
            if solo.count > maxBytes {
                throw ConfigFrameError.rowTooLarge(index: i, bytes: solo.count, maxBytes: maxBytes)
            }
            let candidate = current + [row]
            let probe = try encode(rows: candidate, rev: rev, part: 0, parts: max(rows.count, 1),
                                   maxBytes: maxBytes, checkFit: false)
            if probe.count <= maxBytes {
                current = candidate
            } else {
                groups.append(current)
                current = [row]
            }
        }
        if !current.isEmpty { groups.append(current) }
        return groups
    }

    private static func encode(rows: [TickerRow], rev: UInt32, part: Int, parts: Int,
                               maxBytes: Int, checkFit: Bool = true) throws -> Data {
        let config = ConfigPayload(rev: rev, part: part, parts: parts, tickers: rows)
        let enc = JSONEncoder()
        enc.outputFormatting = [.sortedKeys]   // deterministic for tests; device is key-order-agnostic
        var data = try enc.encode(ConfigEnvelope(v: 1, config: config))
        data.append(0x0A)                       // newline-delimited framing (matches StatusFrame.encoded)
        if checkFit && data.count > maxBytes {
            throw ConfigFrameError.rowTooLarge(index: part, bytes: data.count, maxBytes: maxBytes)
        }
        return data
    }

    // Wire envelope: {"v":1,"config":{"rev","part","parts","tickers":[...]}}.
    private struct ConfigEnvelope: Encodable { let v: Int; let config: ConfigPayload }
    private struct ConfigPayload: Encodable {
        let rev: UInt32; let part: Int; let parts: Int; let tickers: [TickerRow]
    }
}
