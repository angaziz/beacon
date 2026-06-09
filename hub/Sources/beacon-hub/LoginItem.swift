import ServiceManagement

// Thin wrapper over SMAppService.mainApp (issue #16): auto-start the hub at login via a toggleable
// login item. Isolated here because SMAppService is only meaningful from a real .app bundle (it keys
// the registration to the bundle's LaunchServices identity), and so the ad-hoc-signing caveat lives in
// ONE place: under `codesign --sign -` the cdhash changes on every build-app.sh rebuild, so macOS may
// surface a "requires approval" status (Login Items) or leave a stale BTM record -- a Developer-ID /
// notarized build is what makes this fully stable.
enum LoginItem {
    static var status: SMAppService.Status { SMAppService.mainApp.status }
    static var isEnabled: Bool { SMAppService.mainApp.status == .enabled }

    static func setEnabled(_ on: Bool) throws {
        if on { try SMAppService.mainApp.register() }
        else { try SMAppService.mainApp.unregister() }
    }
}
