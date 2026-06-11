import CoreLocation
import Foundation
import AppKit
import BeaconHubKit

// One-shot CoreLocation source for the device place name (issue #54). macOS WiFi positioning gives
// tens-of-meters accuracy with no API key; CLGeocoder yields an Apple Maps place name; tz is free via
// TimeZone.current. We deliberately do NOT monitor continuously -- devs rarely move -- so we request a
// single fix on launch and on Mac wake, reverse-geocode only when the coords meaningfully changed, and
// hand the resulting Loc to onFix. Permission denied / no fix => never fires => the device keeps its IP
// fallback. @MainActor so onFix mutations land on the app's main actor; the CLGeocoder completion (an
// arbitrary queue) hops back explicitly.
@MainActor
final class LocationProvider: NSObject, CLLocationManagerDelegate {
    var onFix: ((Loc) -> Void)?

    private let manager = CLLocationManager()
    private let geocoder = CLGeocoder()
    private var lastGeocoded: CLLocationCoordinate2D?   // coords of the last name we resolved
    private var cachedName: String?
    private var pendingRequest = false                  // a requestLocation() is in flight

    private static let moveThresholdDeg = 0.01          // ~1 km; below this we reuse the cached name

    override init() {
        super.init()
        manager.delegate = self
        manager.desiredAccuracy = kCLLocationAccuracyHundredMeters
        // Re-request a fix when the Mac wakes (the device may have travelled with a closed lid).
        NSWorkspace.shared.notificationCenter.addObserver(
            self, selector: #selector(didWake), name: NSWorkspace.didWakeNotification, object: nil)
    }

    // Kick off authorization; the actual requestLocation() is driven from the authorization callback so
    // we never call it while status is .notDetermined (which silently no-ops on macOS).
    func start() {
        manager.requestWhenInUseAuthorization()
        requestIfAuthorized()
    }

    @objc private func didWake() { requestIfAuthorized() }

    private func requestIfAuthorized() {
        switch manager.authorizationStatus {
        case .authorizedAlways, .authorizedWhenInUse:
            guard !pendingRequest else { return }
            pendingRequest = true
            manager.requestLocation()
        default:
            break   // .notDetermined waits for the prompt; .denied/.restricted => IP fallback
        }
    }

    // MARK: CLLocationManagerDelegate

    nonisolated func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        Task { @MainActor in self.requestIfAuthorized() }
    }

    nonisolated func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        guard let loc = locations.last else { return }
        let reduced = manager.accuracyAuthorization == .reducedAccuracy
        Task { @MainActor in self.handle(loc, reduced: reduced) }
    }

    nonisolated func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        Task { @MainActor in self.pendingRequest = false }   // no fix => emit nothing => device keeps IP fallback
    }

    private func handle(_ location: CLLocation, reduced: Bool) {
        pendingRequest = false
        let coord = location.coordinate
        // Skip the geocode when we haven't meaningfully moved since the last resolved name.
        if let last = lastGeocoded, let name = cachedName,
           abs(last.latitude - coord.latitude) <= Self.moveThresholdDeg,
           abs(last.longitude - coord.longitude) <= Self.moveThresholdDeg {
            emit(coord: coord, name: name)
            return
        }
        geocoder.reverseGeocodeLocation(location) { [weak self] placemarks, _ in
            // Completion runs off the main actor; hop back before touching state / onFix.
            Task { @MainActor in
                guard let self else { return }
                let name = Self.placeName(placemarks?.first, reduced: reduced) ?? self.cachedName ?? ""
                guard !name.isEmpty else { return }
                self.lastGeocoded = coord
                self.cachedName = name
                self.emit(coord: coord, name: name)
            }
        }
    }

    private func emit(coord: CLLocationCoordinate2D, name: String) {
        onFix?(Loc(lat: coord.latitude, lon: coord.longitude,
                   tz: TimeZone.current.identifier, name: name))
    }

    // "<subLocality>, <locality>" when both are present (e.g. "Sukajadi, Bandung"); reduced accuracy
    // (~5 km) drops the sub-locality and reports the city only.
    private static func placeName(_ p: CLPlacemark?, reduced: Bool) -> String? {
        guard let p else { return nil }
        let city = p.locality ?? p.subAdministrativeArea ?? p.administrativeArea
        if !reduced, let sub = p.subLocality, let city, sub != city { return "\(sub), \(city)" }
        return city ?? p.subLocality ?? p.name
    }
}
