import XCTest
@testable import BeaconHubKit

// Pre-add test-fetch parse checks (issue #92). Mirrors parse_finance.cpp: a body is "live" only when the
// device would successfully parse a price from it. Inlined JSON bodies cover real shapes + error/malformed.
final class TickerValidationTests: XCTestCase {
    private func data(_ s: String) -> Data { Data(s.utf8) }

    func testBinanceOK() {
        let cases: [(name: String, body: String, expected: Bool)] = [
            ("live numeric lastPrice", #"{"symbol":"BTCUSDT","lastPrice":"62392.10","priceChangePercent":"1.5"}"#, true),
            ("empty lastPrice",        #"{"symbol":"BTCUSDT","lastPrice":""}"#, false),
            ("missing lastPrice",      #"{"symbol":"BTCUSDT","priceChangePercent":"1.5"}"#, false),
            ("non-numeric lastPrice",  #"{"lastPrice":"n/a"}"#, false),
            ("error body",             #"{"code":-1121,"msg":"Invalid symbol."}"#, false),
            ("malformed json",         #"{"lastPrice":"#, false),
            ("empty body",             "", false),
        ]
        for c in cases {
            XCTAssertEqual(TickerValidation.binanceOK(data(c.body)), c.expected, c.name)
        }
    }

    func testYahooOK() {
        let cases: [(name: String, body: String, expected: Bool)] = [
            ("live regularMarketPrice", #"{"chart":{"result":[{"meta":{"regularMarketPrice":451.23}}],"error":null}}"#, true),
            ("result null",            #"{"chart":{"result":null,"error":{"code":"Not Found"}}}"#, false),
            ("error payload",          #"{"chart":{"result":null,"error":{"code":"Not Found","description":"No data found, symbol may be delisted"}}}"#, false),
            ("meta without price",     #"{"chart":{"result":[{"meta":{"currency":"USD"}}],"error":null}}"#, false),
            ("price null",             #"{"chart":{"result":[{"meta":{"regularMarketPrice":null}}]}}"#, false),
            ("price non-numeric",      #"{"chart":{"result":[{"meta":{"regularMarketPrice":"N/A"}}]}}"#, false),
            ("empty result array",     #"{"chart":{"result":[]}}"#, false),
            ("malformed json",         #"{"chart":{"result":["#, false),
            ("empty body",             "", false),
        ]
        for c in cases {
            XCTAssertEqual(TickerValidation.yahooOK(data(c.body)), c.expected, c.name)
        }
    }
}
