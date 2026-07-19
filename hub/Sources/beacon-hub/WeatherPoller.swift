import Foundation
import BeaconHubKit

// Polls Open-Meteo for current conditions on the device's behalf (CONTRACT.md §A) so the device can
// serve Home with its Wi-Fi radio down. Coordinates come from LocationProvider -- the same fix that
// feeds the `loc` block -- so no additional macOS permission is involved.
//
// Cadence matches the device's own WEATHER_CADENCE_S (600 s): this MOVES the upstream request from the
// device to the Mac rather than adding one. Weather is public data, so unlike UsagePoller there is no
// secret to keep here; the split exists purely to keep the device's radio down.
final class WeatherPoller {
    var onWeather: ((Weather) -> Void)?

    // Last successful result, replayed on (re)connect so a freshly-connected device does not wait out
    // a full cadence for its first reading.
    private(set) var last: Weather?

    private let interval: TimeInterval = 600
    private let session: URLSession
    private var timer: DispatchSourceTimer?
    private let queue = DispatchQueue(label: "beacon.weather")

    // Confined to `queue`.
    private var coord: (lat: Double, lon: Double)?
    private var connected = true
    private var lastFetchAt = Date.distantPast

    init(session: URLSession? = nil) {
        if let session {
            self.session = session
        } else {
            let cfg = URLSessionConfiguration.ephemeral
            cfg.timeoutIntervalForRequest = 15
            self.session = URLSession(configuration: cfg)
        }
    }

    // No fix => nothing is polled and no frame is sent, so the device keeps using its own fetch. A
    // meaningful move refetches immediately rather than waiting out the cadence, since the reading is
    // now for the wrong place.
    func setCoordinate(lat: Double, lon: Double) {
        queue.async { [weak self] in
            guard let self else { return }
            let moved = self.coord.map { abs($0.lat - lat) > 0.01 || abs($0.lon - lon) > 0.01 } ?? true
            self.coord = (lat, lon)
            if moved { self.lastFetchAt = .distantPast; self.tick() }
        }
    }

    // No live device => no reason to hold a 10-minute upstream poll open (mirrors UsagePoller's
    // disconnected gate). On reconnect the cached `last` is replayed by the caller.
    func setDeviceConnected(_ b: Bool) { queue.async { [weak self] in self?.connected = b } }

    func start() {
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now(), repeating: interval, leeway: .seconds(30))   // coalesce wakeups
        t.setEventHandler { [weak self] in self?.tick() }
        t.resume()
        timer = t
    }

    private func tick() {
        guard connected, let coord else { return }
        guard Date().timeIntervalSince(lastFetchAt) >= interval - 1 else { return }   // setCoordinate can re-enter
        guard let url = WeatherNormalizer.requestURL(lat: coord.lat, lon: coord.lon) else { return }
        lastFetchAt = Date()

        session.dataTask(with: url) { [weak self] data, response, _ in
            guard let self,
                  let data,
                  (response as? HTTPURLResponse).map({ (200..<300).contains($0.statusCode) }) == true,
                  // Stamp the moment the fetch succeeded: this is what the device ages against, so it
                  // must not be the frame's send time.
                  let w = WeatherNormalizer.normalize(data, ts: Int(Date().timeIntervalSince1970))
            else { return }   // transient failure => send nothing; the device keeps its last values
            self.queue.async {
                self.last = w
                self.onWeather?(w)
            }
        }.resume()
    }
}
