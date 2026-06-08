// swift-tools-version: 5.9
import PackageDescription
import Foundation

// Absolute path to this package dir, so the linker can embed Info.plist regardless of cwd.
let pkgDir = URL(fileURLWithPath: #filePath).deletingLastPathComponent().path

// Beacon macOS hub (P2). A menubar agent that polls Claude/Codex usage, ingests Claude Code hooks,
// and bridges both to the device over BLE (tech.md §7). Built as a SwiftPM package so it builds from
// the CLI (`swift build` / `swift test`) and opens in Xcode (double-click Package.swift). The agent
// target uses only system frameworks (CoreBluetooth, Network, AppKit, Security) -- no third-party deps.
//
// Build + run via `./build-app.sh` then `"./Beacon Hub.app/Contents/MacOS/beacon-hub"`. A signed .app
// bundle is REQUIRED at runtime: macOS TCC keys Bluetooth permission to a code-signed bundle, so a
// bare `swift run` binary is aborted as a privacy violation. (`swift build`/`swift test` still work
// from the CLI for compile/unit checks.) D4 noted SwiftUI MenuBarExtra + Xcode; this package opens in
// Xcode and is CLI-verifiable, which a hand-generated .xcodeproj is not.
let package = Package(
    name: "BeaconHub",
    platforms: [.macOS(.v13)],
    targets: [
        // Pure, dependency-free logic (the §7.1 frame, usage normalization). Unit-tested on the host.
        .target(name: "BeaconHubKit"),
        // The menubar agent: BLE central, usage pollers, Claude Code hook bridge, menubar UI.
        .executableTarget(
            name: "beacon-hub",
            dependencies: ["BeaconHubKit"],
            // Embed Info.plist into the binary so CoreBluetooth has NSBluetoothAlwaysUsageDescription
            // (a bare CLI executable has no bundle; without it macOS TCC aborts on first BLE access).
            linkerSettings: [
                .unsafeFlags([
                    "-Xlinker", "-sectcreate",
                    "-Xlinker", "__TEXT",
                    "-Xlinker", "__info_plist",
                    "-Xlinker", "\(pkgDir)/Info.plist",
                ])
            ]
        ),
        .testTarget(name: "BeaconHubKitTests", dependencies: ["BeaconHubKit"]),
    ]
)
