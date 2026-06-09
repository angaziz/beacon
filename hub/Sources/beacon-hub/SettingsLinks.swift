import AppKit
import Foundation

// Single source for the System Settings deep links so MenubarController and FirstRunWindowController
// can't drift. macOS 13+ pane ids; the pre-Ventura "com.apple.Bluetooth" no longer resolves.
enum SettingsLinks {
    static let bluetooth = URL(string: "x-apple.systempreferences:com.apple.BluetoothSettings")!
    static let privacyBluetooth = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Bluetooth")!
    static let fallback = URL(string: "x-apple.systempreferences:")!

    // Open `url`, falling back to the Settings root if a drifted pane id fails to resolve.
    static func open(_ url: URL) {
        if NSWorkspace.shared.open(url) { return }
        NSWorkspace.shared.open(fallback)
    }
}
