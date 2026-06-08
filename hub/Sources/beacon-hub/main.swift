import AppKit

// Menubar agent entry point. .accessory => no Dock icon / app bundle needed for development.
// AppKit + AppDelegate are main-actor isolated; the bootstrap runs there explicitly.
MainActor.assumeIsolated {
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    let delegate = AppDelegate()
    app.delegate = delegate
    app.run()
}
