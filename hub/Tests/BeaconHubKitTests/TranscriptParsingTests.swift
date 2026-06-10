import XCTest
@testable import BeaconHubKit

final class TranscriptParsingTests: XCTestCase {
    private func obj(_ json: String) -> [String: Any] {
        try! JSONSerialization.jsonObject(with: Data(json.utf8)) as! [String: Any]
    }

    func testClaudeAssistantLine() {
        let line = obj(#"""
        {"type":"assistant","requestId":"req_1","uuid":"u-1","timestamp":"2026-06-10T11:13:54.726Z",
         "message":{"model":"claude-opus-4-8","usage":{"input_tokens":12902,"cache_read_input_tokens":16371,
         "output_tokens":229,"cache_creation":{"ephemeral_5m_input_tokens":0,"ephemeral_1h_input_tokens":3106}}}}
        """#)
        let e = ClaudeTranscriptParser.parseLine(line)
        XCTAssertEqual(e?.dedupKey, "req_1")
        XCTAssertEqual(e?.model, "claude-opus-4-8")
        XCTAssertEqual(e?.usage.inputTokens, 12902)
        XCTAssertEqual(e?.usage.cacheReadTokens, 16371)
        XCTAssertEqual(e?.usage.cacheWrite1hTokens, 3106)
        XCTAssertEqual(e?.usage.cacheWrite5mTokens, 0)
        XCTAssertEqual(e?.usage.outputTokens, 229)
        XCTAssertEqual(e?.epochSeconds, 1_781_090_034)   // 2026-06-10T11:13:54Z floored (verified)
    }

    func testClaudeFallsBackToUuidWhenNoRequestId() {
        let line = obj(#"{"type":"assistant","uuid":"u-2","timestamp":"2026-06-10T11:13:54Z","message":{"model":"sonnet","usage":{"input_tokens":2,"output_tokens":5}}}"#)
        XCTAssertEqual(ClaudeTranscriptParser.parseLine(line)?.dedupKey, "u-2")
        XCTAssertEqual(ClaudeTranscriptParser.parseLine(line)?.model, "sonnet")
    }

    func testClaudeSkipsSyntheticAndNonAssistant() {
        let synthetic = obj(#"{"type":"assistant","requestId":"r","timestamp":"2026-06-10T11:13:54Z","message":{"model":"<synthetic>","usage":{"input_tokens":1}}}"#)
        XCTAssertNil(ClaudeTranscriptParser.parseLine(synthetic))
        let user = obj(#"{"type":"user","uuid":"x","message":{"model":"claude-opus-4-8"}}"#)
        XCTAssertNil(ClaudeTranscriptParser.parseLine(user))
    }

    func testCodexTokenCountDelta() {
        let line = obj(#"""
        {"type":"event_msg","timestamp":"2026-05-31T14:25:03Z","payload":{"type":"token_count","info":{
          "last_token_usage":{"input_tokens":15510,"cached_input_tokens":3456,"output_tokens":426,
            "reasoning_output_tokens":277,"total_tokens":15936},
          "total_token_usage":{"input_tokens":15510,"output_tokens":426}}}}
        """#)
        let e = CodexTranscriptParser.parseLine(line)
        XCTAssertEqual(e?.usage.inputTokens, 15510)
        XCTAssertEqual(e?.usage.cachedInputTokens, 3456)
        XCTAssertEqual(e?.usage.outputTokens, 426)
        XCTAssertEqual(e?.usage.reasoningOutputTokens, 277)
        XCTAssertEqual(e?.usage.totalTokens, 15936)   // verified invariant: input + output
    }

    func testCodexSkipsNullInfo() {
        let line = obj(#"{"type":"event_msg","timestamp":"2026-06-10T11:13:54Z","payload":{"type":"token_count","info":null,"rate_limits":{"primary":null}}}"#)
        XCTAssertNil(CodexTranscriptParser.parseLine(line))
    }

    func testCodexModelFromTurnContext() {
        let line = obj(#"{"type":"turn_context","payload":{"model":"gpt-5.5","cwd":"/x"}}"#)
        XCTAssertEqual(CodexTranscriptParser.parseModel(line), "gpt-5.5")
        XCTAssertNil(CodexTranscriptParser.parseModel(obj(#"{"type":"event_msg","payload":{}}"#)))
    }

    func testCodexBackfillSample() {
        let line = obj(#"""
        {"type":"event_msg","timestamp":"2026-05-31T14:25:09Z","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":1,"output_tokens":1,"total_tokens":2}},
          "rate_limits":{"primary":{"used_percent":1.0,"window_minutes":300,"resets_at":1780230309},
            "secondary":{"used_percent":92.0,"window_minutes":10080,"resets_at":1780230956}}}}
        """#)
        let s = CodexTranscriptParser.parseRateLimits(line)
        XCTAssertEqual(s?.h5Pct, 1)
        XCTAssertEqual(s?.h5Reset, 1_780_230_309)
        XCTAssertEqual(s?.d7Pct, 92)
        XCTAssertEqual(s?.d7Reset, 1_780_230_956)
    }
}
