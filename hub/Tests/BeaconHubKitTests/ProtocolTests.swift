import XCTest
@testable import BeaconHubKit

final class ProtocolTests: XCTestCase {

    func testStatusFrameEncodesV1AndNewline() throws {
        let usage = Usage(
            claude: ProviderUsage(h5: UsageWindow(pct: 24, reset: 1717600000),
                                  d7: UsageWindow(pct: 24, reset: 1717800000)),
            codex: ProviderUsage(h5: UsageWindow(pct: 1, reset: 1717590000),
                                 d7: UsageWindow(pct: 29, reset: 1717800000)))
        let buddy = BuddyState(running: 2, waiting: 1, tokens: 184502, contextPct: 42,
                               entries: ["10:42 git push"],
                               prompt: BuddyPrompt(id: "req_abc", tool: "Bash", hint: "rm -rf /tmp/build"))
        let data = try StatusFrame(usage: usage, buddy: buddy).encoded()
        XCTAssertEqual(data.last, 0x0A)  // newline-terminated
        let obj = try JSONSerialization.jsonObject(with: data) as! [String: Any]
        XCTAssertEqual(obj["v"] as? Int, 1)
        let claude = ((obj["usage"] as! [String: Any])["claude"] as! [String: Any])
        XCTAssertEqual((claude["h5"] as! [String: Any])["pct"] as? Int, 24)
        let b = obj["buddy"] as! [String: Any]
        XCTAssertEqual(b["context_pct"] as? Int, 42)
        XCTAssertNotNil(b["prompt"])
    }

    func testNilPctOmitted() throws {
        let usage = Usage(claude: ProviderUsage(h5: UsageWindow(pct: nil, reset: 0),
                                                d7: UsageWindow(pct: 50, reset: 1)),
                          codex: .unavailable)
        let data = try StatusFrame(usage: usage).encoded()
        let obj = try JSONSerialization.jsonObject(with: data) as! [String: Any]
        let h5 = (((obj["usage"] as! [String: Any])["claude"] as! [String: Any])["h5"] as! [String: Any])
        XCTAssertNil(h5["pct"])               // nil pct omitted; device reads it as unavailable
        XCTAssertEqual(h5["reset"] as? Int, 0)
    }

    func testIdleFrameHasNoPrompt() throws {
        let data = try StatusFrame(buddy: BuddyState(running: 0, waiting: 0)).encoded()
        let obj = try JSONSerialization.jsonObject(with: data) as! [String: Any]
        XCTAssertNil((obj["buddy"] as! [String: Any])["prompt"])  // absence of prompt => idle
    }

    func testLocFrameEncodesAndOmitsOtherBlocks() throws {
        // A loc-only on-change frame: loc present, usage/buddy absent (the device keeps their values).
        let fix = Loc(lat: -6.91, lon: 107.61, tz: "Asia/Jakarta", name: "Sukajadi, Bandung")
        let obj = try JSONSerialization.jsonObject(with: StatusFrame(loc: fix).encoded()) as! [String: Any]
        XCTAssertEqual(obj["v"] as? Int, 1)
        XCTAssertNil(obj["usage"]); XCTAssertNil(obj["buddy"])
        let loc = obj["loc"] as! [String: Any]
        XCTAssertEqual(loc["lat"] as? Double, -6.91)
        XCTAssertEqual(loc["lon"] as? Double, 107.61)
        XCTAssertEqual(loc["tz"] as? String, "Asia/Jakarta")
        XCTAssertEqual(loc["name"] as? String, "Sukajadi, Bandung")
    }

    func testHeartbeatFrameOmitsLoc() throws {
        // The heartbeat full frame must NOT carry loc (issue #54): loc rides connect/on-change only.
        let obj = try JSONSerialization.jsonObject(
            with: StatusFrame(usage: .init(claude: .unavailable, codex: .unavailable),
                              buddy: BuddyState()).encoded()) as! [String: Any]
        XCTAssertNil(obj["loc"])
    }

    func testLocRoundTrips() throws {
        let fix = Loc(lat: 1.5, lon: 2.5, tz: "UTC", name: "Nowhere")
        let back = try JSONDecoder().decode(Loc.self, from: JSONEncoder().encode(fix))
        XCTAssertEqual(back, fix)
    }

    func testParsePermission() {
        let approve = DeviceCommand.parse(Data(#"{"v":1,"cmd":"permission","id":"req_abc","decision":"approve"}"#.utf8))
        XCTAssertEqual(approve, .permission(id: "req_abc", approve: true))
        let deny = DeviceCommand.parse(Data(#"{"v":1,"cmd":"permission","id":"x","decision":"deny"}"#.utf8))
        XCTAssertEqual(deny, .permission(id: "x", approve: false))
    }

    func testParseRejectsBadVersionOrCmd() {
        XCTAssertNil(DeviceCommand.parse(Data(#"{"v":2,"cmd":"permission","id":"x","decision":"deny"}"#.utf8)))
        XCTAssertNil(DeviceCommand.parse(Data(#"{"v":1,"cmd":"nope"}"#.utf8)))
        XCTAssertNil(DeviceCommand.parse(Data("garbage".utf8)))
    }

    func testPermissionHookResponseShapePerEvent() throws {
        // PreToolUse uses permissionDecision; PermissionRequest uses decision.behavior. A wrong shape
        // means CC ignores the decision and the device's approve/deny never gates the tool.
        let cases: [(event: String, allow: Bool)] = [
            ("PreToolUse", true), ("PreToolUse", false),
            ("PermissionRequest", true), ("PermissionRequest", false),
        ]
        for c in cases {
            let obj = try JSONSerialization.jsonObject(
                with: HookResponse.permission(event: c.event, allow: c.allow)) as! [String: Any]
            let out = obj["hookSpecificOutput"] as! [String: Any]
            XCTAssertEqual(out["hookEventName"] as? String, c.event, "\(c)")
            switch c.event {
            case "PermissionRequest":
                XCTAssertNil(out["permissionDecision"], "PermissionRequest must not use the PreToolUse shape \(c)")
                let decision = out["decision"] as! [String: Any]
                XCTAssertEqual(decision["behavior"] as? String, c.allow ? "allow" : "deny", "\(c)")
                XCTAssertEqual(decision["message"] != nil, !c.allow, "message only on deny \(c)")
            default:
                XCTAssertNil(out["decision"], "PreToolUse must not use the PermissionRequest shape \(c)")
                XCTAssertEqual(out["permissionDecision"] as? String, c.allow ? "allow" : "deny", "\(c)")
            }
        }
    }

    func testPermissionAskShapePerEvent() throws {
        // Defer to CC's own prompt (AskUserQuestion passthrough). PreToolUse's permissionDecision supports
        // "ask"; PermissionRequest's decision.behavior does NOT (allow/deny only), so it must emit no
        // decision -- an empty object CC reads as "no gate", falling through to its interactive prompt.
        let pr = try JSONSerialization.jsonObject(
            with: HookResponse.permissionAsk(event: "PermissionRequest")) as! [String: Any]
        XCTAssertTrue(pr.isEmpty, "PermissionRequest ask must emit no decision (got \(pr))")

        let pre = try JSONSerialization.jsonObject(
            with: HookResponse.permissionAsk(event: "PreToolUse")) as! [String: Any]
        let out = pre["hookSpecificOutput"] as! [String: Any]
        XCTAssertEqual(out["hookEventName"] as? String, "PreToolUse")
        XCTAssertEqual(out["permissionDecision"] as? String, "ask")
        XCTAssertNil(out["decision"], "PreToolUse must not use the PermissionRequest shape")
    }

    func testPermissionDenyMessageNamesCause() throws {
        // A custom deny message (e.g. "Beacon device offline") must surface in the TUI for both event
        // shapes; nil falls back to the generic reason; allow never carries a message.
        func reason(_ obj: [String: Any], event: String) -> String? {
            // The user-visible reason: PermissionRequest carries it in decision.message (deny only),
            // PreToolUse in permissionDecisionReason (always).
            let out = obj["hookSpecificOutput"] as! [String: Any]
            if event == "PermissionRequest" { return (out["decision"] as? [String: Any])?["message"] as? String }
            return out["permissionDecisionReason"] as? String
        }
        let cases: [(event: String, allow: Bool, message: String?, want: String?)] = [
            ("PermissionRequest", false, "Beacon device offline", "Beacon device offline"),
            ("PermissionRequest", false, "another prompt is pending", "another prompt is pending"),
            ("PermissionRequest", false, nil, "Denied on Beacon device"),
            ("PermissionRequest", true, "Beacon device offline", nil),   // allow ignores message
            ("PreToolUse", false, "Beacon device offline", "Beacon device offline"),
            ("PreToolUse", false, nil, "Denied on Beacon device"),
            ("PreToolUse", true, "Beacon device offline", "Approved on Beacon device"),
        ]
        for c in cases {
            let obj = try JSONSerialization.jsonObject(
                with: HookResponse.permission(event: c.event, allow: c.allow, message: c.message)) as! [String: Any]
            XCTAssertEqual(reason(obj, event: c.event), c.want, "\(c)")
        }
    }

    func testAckAndErr() throws {
        let ack = try JSONSerialization.jsonObject(with: HubAck.ack(id: "req_abc", ok: true)) as! [String: Any]
        XCTAssertEqual(ack["v"] as? Int, 1)
        XCTAssertEqual(ack["ack"] as? String, "req_abc")
        XCTAssertEqual(ack["ok"] as? Bool, true)
        // ok:false = decision did not apply (late/superseded); same shape, ok flips to false (issue #8).
        let nack = try JSONSerialization.jsonObject(with: HubAck.ack(id: "req_abc", ok: false)) as! [String: Any]
        XCTAssertEqual(nack["ack"] as? String, "req_abc")
        XCTAssertEqual(nack["ok"] as? Bool, false)
        let err = try JSONSerialization.jsonObject(with: HubAck.err(id: "req_xyz", reason: "unknown_prompt_id")) as! [String: Any]
        XCTAssertEqual(err["err"] as? String, "unknown_prompt_id")
        XCTAssertEqual(err["id"] as? String, "req_xyz")
    }
}

final class UsageNormalizerTests: XCTestCase {

    // Real redacted capture from GET api.anthropic.com/api/oauth/usage (CONTRACT.md §C.1 fallback,
    // 2026-06-11). resets_at is microsecond-precision ISO with a +00:00 offset (not a clean Z), and
    // the body carries extra windows (seven_day_sonnet, extra_usage, ...) the normalizer must ignore.
    func testClaudeNormalization() {
        let raw = Data(#"""
        {"five_hour":{"utilization":8.0,"resets_at":"2026-06-11T03:30:00.110763+00:00"},
         "seven_day":{"utilization":32.0,"resets_at":"2026-06-15T00:00:01.110782+00:00"},
         "seven_day_sonnet":{"utilization":2.0,"resets_at":"2026-06-15T00:00:01.110788+00:00"},
         "extra_usage":{"is_enabled":false,"utilization":null}}
        """#.utf8)
        let p = UsageNormalizer.claude(raw)
        XCTAssertEqual(p?.h5.pct, 8)
        XCTAssertEqual(p?.d7.pct, 32)
        XCTAssertEqual(p?.h5.reset, 1781148600)     // microsecond ISO + offset -> epoch
        XCTAssertEqual(p?.d7.reset, 1781481601)
    }

    // Real redacted capture from GET chatgpt.com/backend-api/wham/usage (CONTRACT.md §C.2, 2026-06-11).
    // used_percent arrives as an Int here; extra fields (allowed, limit_window_seconds, credits, ...)
    // must be ignored. The draft P2-0 guess matched the live shape on every field read here.
    func testCodexNormalization() {
        let raw = Data(#"""
        {"plan_type":"plus",
         "rate_limit":{"allowed":false,"limit_reached":true,
           "primary_window":{"used_percent":1,"limit_window_seconds":18000,"reset_after_seconds":18000,"reset_at":1781151661},
           "secondary_window":{"used_percent":100,"limit_window_seconds":604800,"reset_after_seconds":15234,"reset_at":1781148895}},
         "credits":{"has_credits":false,"balance":"0"}}
        """#.utf8)
        let p = UsageNormalizer.codex(raw)
        XCTAssertEqual(p?.h5.pct, 1)
        XCTAssertEqual(p?.h5.reset, 1781151661)
        XCTAssertEqual(p?.d7.pct, 100)
        XCTAssertEqual(p?.d7.reset, 1781148895)
    }

    func testMalformedReturnsNil() {
        XCTAssertNil(UsageNormalizer.claude(Data("{}".utf8)))
        XCTAssertNil(UsageNormalizer.codex(Data("not json".utf8)))
    }
}
