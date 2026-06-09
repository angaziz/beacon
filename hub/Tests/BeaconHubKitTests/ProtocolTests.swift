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
        // "ask" defers to CC's own prompt (AskUserQuestion passthrough): no behavior=allow/deny, no
        // message. Same per-event split as a real decision (decision.behavior vs permissionDecision).
        let cases: [(event: String, wantBehavior: String?, wantDecision: String?)] = [
            ("PermissionRequest", "ask", nil),
            ("PreToolUse", nil, "ask"),
        ]
        for c in cases {
            let obj = try JSONSerialization.jsonObject(
                with: HookResponse.permissionAsk(event: c.event)) as! [String: Any]
            let out = obj["hookSpecificOutput"] as! [String: Any]
            XCTAssertEqual(out["hookEventName"] as? String, c.event, "\(c)")
            XCTAssertEqual((out["decision"] as? [String: Any])?["behavior"] as? String, c.wantBehavior, "\(c)")
            XCTAssertEqual(out["permissionDecision"] as? String, c.wantDecision, "\(c)")
            XCTAssertNil((out["decision"] as? [String: Any])?["message"], "ask carries no deny message \(c)")
        }
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

    func testClaudeNormalization() {
        let raw = Data(#"""
        {"five_hour":{"utilization":24.0,"resets_at":"2026-06-05T14:20:00Z"},
         "seven_day":{"utilization":51.0,"resets_at":"2026-06-07T23:59:00Z"}}
        """#.utf8)
        let p = UsageNormalizer.claude(raw)
        XCTAssertEqual(p?.h5.pct, 24)
        XCTAssertEqual(p?.d7.pct, 51)
        XCTAssertGreaterThan(p?.h5.reset ?? 0, 0)   // ISO -> epoch
    }

    func testCodexNormalization() {
        let raw = Data(#"""
        {"rate_limit":{"primary_window":{"used_percent":1.4,"reset_at":1717590000},
                       "secondary_window":{"used_percent":29.0,"reset_at":1717800000}}}
        """#.utf8)
        let p = UsageNormalizer.codex(raw)
        XCTAssertEqual(p?.h5.pct, 1)               // 1.4 rounds to 1
        XCTAssertEqual(p?.h5.reset, 1717590000)
        XCTAssertEqual(p?.d7.pct, 29)
    }

    func testMalformedReturnsNil() {
        XCTAssertNil(UsageNormalizer.claude(Data("{}".utf8)))
        XCTAssertNil(UsageNormalizer.codex(Data("not json".utf8)))
    }
}
