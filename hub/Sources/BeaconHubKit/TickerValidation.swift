import Foundation

// Pre-add test-fetch validation (issue #92): before a candidate joins the desired list, the hub fetches
// the SAME data endpoint the device will hit (finance.cpp) and confirms a live, parseable instrument.
// This catches catalog symbols whose 24hr/chart endpoint stalls or returns nothing -- adding such a row
// previously contributed to a device watchdog reboot. PURE PARSE ONLY: these mirror parse_finance.cpp
// (parse_binance / parse_yahoo) and return false for any missing/empty/non-numeric/malformed body.
public enum TickerValidation {
    // Binance 24hr body is live iff "lastPrice" is a non-empty string parseable as a number. Mirrors
    // parse_binance: numbers are JSON strings, and an error body ({"code","msg"}) has no lastPrice.
    public static func binanceOK(_ body: Data) -> Bool {
        guard let obj = try? JSONSerialization.jsonObject(with: body) as? [String: Any],
              let lastPrice = obj["lastPrice"] as? String, !lastPrice.isEmpty,
              Double(lastPrice) != nil
        else { return false }
        return true
    }

    // Yahoo chart body is live iff chart.result[0].meta.regularMarketPrice is a number. Mirrors
    // parse_yahoo's null checks; a result-null / error payload or a meta missing the price fails.
    public static func yahooOK(_ body: Data) -> Bool {
        guard let obj = try? JSONSerialization.jsonObject(with: body) as? [String: Any],
              let chart = obj["chart"] as? [String: Any],
              let result = chart["result"] as? [[String: Any]],
              let meta = result.first?["meta"] as? [String: Any],
              meta["regularMarketPrice"] is NSNumber
        else { return false }
        return true
    }
}
