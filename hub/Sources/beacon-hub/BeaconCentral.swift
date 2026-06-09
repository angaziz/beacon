import Foundation
import CoreBluetooth
import BeaconHubKit

// Distinct link phases the UI distinguishes (issue #11). Rare CBManager states (.resetting/.unsupported/
// .unknown) fold into .unavailable since none has a distinct user remediation.
enum LinkPhase: Equatable {
    case bluetoothOff
    case unauthorized
    case unavailable
    case searching
    case connecting(String)
    case connected(String)
    case reconnecting
}

// CoreBluetooth central for the Nordic-UART link to the device (tech.md 7.1). Scans for the Beacon
// peripheral, subscribes TX (device->hub notify), writes RX (hub->device), reassembles inbound lines
// on '\n', and parses each into a DeviceCommand. Pairing is OS-mediated: simply accessing the
// encrypted TX/RX characteristic makes macOS surface its pairing dialog -- we never touch passkeys.
final class BeaconCentral: NSObject {

    // NUS UUIDs (tech.md 7.1). RX = hub->device write; TX = device->hub notify.
    private static let service = CBUUID(string: "6e400001-b5a3-f393-e0a9-e50e24dcca9e")
    private static let rxChar  = CBUUID(string: "6e400002-b5a3-f393-e0a9-e50e24dcca9e")
    private static let txChar  = CBUUID(string: "6e400003-b5a3-f393-e0a9-e50e24dcca9e")
    private static let namePrefix = "Beacon"

    // Callbacks (set by AppDelegate). onReady => caller resends a full frame after (re)subscribe.
    // onPhaseChange carries a LinkPhase computed ON this queue, so callers never read the queue-owned
    // link state back across threads.
    var onReady: (() -> Void)?
    var onCommand: ((DeviceCommand) -> Void)?
    var onPhaseChange: ((LinkPhase) -> Void)?

    private(set) var isConnected = false {
        didSet {
            guard oldValue != isConnected else { return }
            // Disconnect path (handleDisconnect => beginScan) owns the next phase; only the rising edge
            // emits .connected here.
            if isConnected { hadConnection = true; setPhase(.connected(connectedName ?? "")) }
        }
    }
    private(set) var connectedName: String?

    private var phase: LinkPhase? = nil   // nil sentinel: forces first emit even if first real state folds to .unavailable
    private var hadConnection = false
    private func setPhase(_ p: LinkPhase) {
        guard p != phase else { return }
        phase = p
        onPhaseChange?(p)
    }

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var rx: CBCharacteristic?
    private var tx: CBCharacteristic?

    // Accumulates inbound TX bytes; a frame may span several notifies and a notify several frames.
    private var inbound = Data()
    private let queue = DispatchQueue(label: "beacon.central")

    func start() {
        // Restoration is unused (agent process owns the single link); nil options keep it simple.
        central = CBCentralManager(delegate: self, queue: queue, options: nil)
    }

    // Write a full (already newline-terminated) frame to RX, chunked to the negotiated MTU. We use
    // .withResponse (acknowledged, flow-controlled): under WiFi+BLE coexistence congestion,
    // .withoutResponse packets are silently dropped/truncated, corrupting multi-chunk frames (the
    // device logs "bad/ignored frame"). Acknowledged writes guarantee delivery + ordering. Our data
    // rate is tiny (a frame every ~30 s + commands), so the extra round-trips are free. Framing is by
    // '\n', so a frame still safely spans multiple writes.
    func send(_ data: Data) {
        queue.async { [weak self] in
            guard let self, let p = self.peripheral, let rx = self.rx, self.isConnected else { return }
            let max = p.maximumWriteValueLength(for: .withResponse)
            guard max > 0 else { return }
            var offset = 0
            while offset < data.count {
                let end = Swift.min(offset + max, data.count)
                p.writeValue(data.subdata(in: offset..<end), for: rx, type: .withResponse)
                offset = end
            }
        }
    }

    private func beginScan() {
        guard central.state == .poweredOn else { return }
        inbound.removeAll(keepingCapacity: true)
        central.scanForPeripherals(withServices: [Self.service], options: nil)
        setPhase(hadConnection ? .reconnecting : .searching)
    }

    private func handleDisconnect() {
        // isConnected=false does not emit a phase; beginScan below emits .reconnecting (hadConnection set).
        connectedName = nil
        isConnected = false
        rx = nil
        tx = nil
        // Error paths (service/char discovery, pairing failure) land here while still GATT-connected;
        // releasing the reference alone leaves CoreBluetooth holding the link, so the device never
        // re-advertises and the rescan below can't rediscover it. Cancel first (no-op if not connected).
        if let p = peripheral { central.cancelPeripheralConnection(p) }
        peripheral = nil
        inbound.removeAll(keepingCapacity: true)
        beginScan()   // auto-reconnect (FR-HUB-3): rescan immediately.
    }

    private func drainInbound() {
        // Split on '\n'; keep any trailing partial in the buffer.
        while let nl = inbound.firstIndex(of: 0x0A) {
            let line = inbound.subdata(in: inbound.startIndex..<nl)
            inbound.removeSubrange(inbound.startIndex...nl)
            guard !line.isEmpty else { continue }
            if let cmd = DeviceCommand.parse(line) {
                let handler = onCommand
                DispatchQueue.main.async { handler?(cmd) }
            }
            // else: malformed/unknown line -> ignore (tech.md 7.1).
        }
    }
}

extension BeaconCentral: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn: beginScan()
        case .poweredOff: isConnected = false; setPhase(.bluetoothOff)
        case .unauthorized: isConnected = false; setPhase(.unauthorized)
        default: isConnected = false; setPhase(.unavailable)
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let advName = (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? peripheral.name
        guard let name = advName, name.hasPrefix(Self.namePrefix) else { return }
        central.stopScan()
        self.peripheral = peripheral
        self.connectedName = name
        peripheral.delegate = self
        central.connect(peripheral, options: nil)
        setPhase(.connecting(name))
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([Self.service])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral,
                        error: Error?) {
        handleDisconnect()
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        handleDisconnect()
    }
}

extension BeaconCentral: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard error == nil, let svc = peripheral.services?.first(where: { $0.uuid == Self.service })
        else { handleDisconnect(); return }
        peripheral.discoverCharacteristics([Self.rxChar, Self.txChar], for: svc)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        guard error == nil, let chars = service.characteristics else { handleDisconnect(); return }
        for c in chars {
            if c.uuid == Self.rxChar { rx = c }
            if c.uuid == Self.txChar {
                tx = c
                // Accessing the encrypted TX char triggers macOS's OS-mediated pairing dialog (D3).
                peripheral.setNotifyValue(true, for: c)
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard characteristic.uuid == Self.txChar else { return }
        if let error = error {
            // Pairing/encryption likely failed -> drop and let auto-reconnect retry.
            _ = error
            handleDisconnect()
            return
        }
        if characteristic.isNotifying && rx != nil {
            isConnected = true
            let ready = onReady
            DispatchQueue.main.async { ready?() }   // caller resends a full status frame.
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard characteristic.uuid == Self.txChar, error == nil,
              let value = characteristic.value else { return }
        inbound.append(value)
        drainInbound()
    }
}
