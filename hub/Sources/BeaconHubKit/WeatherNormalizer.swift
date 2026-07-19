import Foundation

// Open-Meteo current-conditions -> Weather (CONTRACT.md §A). Pure + host-testable, like
// UsageNormalizer: the poller owns URLSession, this owns bytes->record.
//
// Mirrors the device's own fetch/parse_weather.cpp exactly (same endpoint, same three `current`
// fields) so hub-sourced and device-sourced weather are indistinguishable downstream. Keep the two
// in sync -- the device path stays as the fallback for when the hub link is down.
public enum WeatherNormalizer {
    public static let host = "api.open-meteo.com"

    // The request the device would have made. Coordinates come from the hub's CoreLocation fix (the
    // same source as the `loc` block), so no extra permission is involved.
    public static func requestURL(lat: Double, lon: Double) -> URL? {
        var c = URLComponents()
        c.scheme = "https"
        c.host = host
        c.path = "/v1/forecast"
        c.queryItems = [
            URLQueryItem(name: "latitude", value: String(format: "%.4f", lat)),
            URLQueryItem(name: "longitude", value: String(format: "%.4f", lon)),
            URLQueryItem(name: "current", value: "temperature_2m,relative_humidity_2m,weather_code"),
        ]
        return c.url
    }

    // `ts` is passed in rather than read from the clock here so the transform stays pure and the
    // caller can stamp the moment the fetch actually succeeded (that is what the device ages against).
    // Returns nil when the payload is unusable; the caller then sends nothing and the device keeps
    // its last values, matching the "partial block is rejected whole" rule in the contract.
    public static func normalize(_ data: Data, ts: Int) -> Weather? {
        guard let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let cur = obj["current"] as? [String: Any],
              let temp = numeric(cur["temperature_2m"]) else { return nil }
        // Humidity/code are tolerated as absent (Open-Meteo omits fields for some stations); the
        // device's parser likewise defaults them to 0 rather than failing the whole record.
        let rh = numeric(cur["relative_humidity_2m"]) ?? 0
        let wmo = numeric(cur["weather_code"]) ?? 0
        return Weather(temp_c: temp,
                       rh: clampPercent(rh),
                       wmo: max(0, Int(wmo)),
                       ts: ts)
    }

    private static func numeric(_ v: Any?) -> Double? {
        if let d = v as? Double { return d }
        if let i = v as? Int { return Double(i) }
        if let n = v as? NSNumber { return n.doubleValue }
        return nil
    }

    private static func clampPercent(_ v: Double) -> Int {
        if v.isNaN { return 0 }
        return min(100, max(0, Int(v.rounded())))
    }
}
