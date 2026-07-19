import XCTest
@testable import BeaconHubKit

final class WeatherTests: XCTestCase {

    // The exact wire shape the device's hub_parse_weather asserts (CONTRACT.md §A).
    func testEncodesWireShape() throws {
        let s = String(decoding: try WeatherFrame(Weather(temp_c: 21.4, rh: 58, wmo: 3, ts: 1721390000)).encoded(),
                       as: UTF8.self)
        XCTAssertTrue(s.hasSuffix("\n"))
        XCTAssertTrue(s.contains("\"v\":1"))
        XCTAssertTrue(s.contains("\"temp_c\":21.4"))
        XCTAssertTrue(s.contains("\"rh\":58"))
        XCTAssertTrue(s.contains("\"wmo\":3"))
        XCTAssertTrue(s.contains("\"ts\":1721390000"))
    }

    func testFrameIsWellUnderFrameMax() throws {
        let bytes = try WeatherFrame(Weather(temp_c: -273.15, rh: 100, wmo: 99, ts: 2147483647)).encoded().count
        XCTAssertLessThan(bytes, 1024)
    }

    // Mirrors the device's fetch/parse_weather.cpp against the same upstream payload.
    func testNormalizesOpenMeteoCurrent() throws {
        let json = Data("""
        {"latitude":-6.9,"longitude":107.6,
         "current":{"time":"2026-07-19T10:00","temperature_2m":31.8,
                    "relative_humidity_2m":57,"weather_code":2}}
        """.utf8)
        let w = try XCTUnwrap(WeatherNormalizer.normalize(json, ts: 1721390000))
        XCTAssertEqual(w.temp_c, 31.8, accuracy: 0.001)
        XCTAssertEqual(w.rh, 57)
        XCTAssertEqual(w.wmo, 2)
        XCTAssertEqual(w.ts, 1721390000)   // caller's fetch time, never derived from the payload
    }

    // Temperature is the one required field: without it there is nothing worth showing.
    func testRejectsPayloadWithoutTemperature() {
        XCTAssertNil(WeatherNormalizer.normalize(Data("{\"current\":{\"weather_code\":2}}".utf8), ts: 1))
        XCTAssertNil(WeatherNormalizer.normalize(Data("{}".utf8), ts: 1))
        XCTAssertNil(WeatherNormalizer.normalize(Data("not json".utf8), ts: 1))
    }

    // Some stations omit humidity/code; the device's parser defaults them rather than dropping the
    // whole record, so the hub must not be stricter.
    func testToleratesMissingHumidityAndCode() throws {
        let w = try XCTUnwrap(WeatherNormalizer.normalize(
            Data("{\"current\":{\"temperature_2m\":-4}}".utf8), ts: 5))
        XCTAssertEqual(w.temp_c, -4, accuracy: 0.001)
        XCTAssertEqual(w.rh, 0)
        XCTAssertEqual(w.wmo, 0)
    }

    func testClampsHumidityToPercentRange() throws {
        let over = try XCTUnwrap(WeatherNormalizer.normalize(
            Data("{\"current\":{\"temperature_2m\":10,\"relative_humidity_2m\":140}}".utf8), ts: 1))
        XCTAssertEqual(over.rh, 100)
        let under = try XCTUnwrap(WeatherNormalizer.normalize(
            Data("{\"current\":{\"temperature_2m\":10,\"relative_humidity_2m\":-5}}".utf8), ts: 1))
        XCTAssertEqual(under.rh, 0)
    }

    // The URL must stay identical to the device's request in fetch/weather.cpp -- the two paths are
    // interchangeable by design, and a drift here would silently change what the hub reports.
    func testRequestURLMatchesDeviceRequest() throws {
        let url = try XCTUnwrap(WeatherNormalizer.requestURL(lat: -6.91, lon: 107.61))
        XCTAssertEqual(url.host, "api.open-meteo.com")
        XCTAssertEqual(url.path, "/v1/forecast")
        let q = try XCTUnwrap(URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems)
        XCTAssertEqual(q.first { $0.name == "latitude" }?.value, "-6.9100")
        XCTAssertEqual(q.first { $0.name == "longitude" }?.value, "107.6100")
        XCTAssertEqual(q.first { $0.name == "current" }?.value,
                       "temperature_2m,relative_humidity_2m,weather_code")
    }
}
