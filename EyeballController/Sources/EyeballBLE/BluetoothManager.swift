import CoreBluetooth
import Foundation

/// Custom service UUID matching the firmware: 4579ba11-0000-1000-8000-00805f9b34fb
public let eyeballServiceUUID = CBUUID(string: "4579ba11-0000-1000-8000-00805f9b34fb")

/// Device name characteristic UUID (0x0001 within our service)
public let deviceNameCharUUID = CBUUID(string: "0001")

public class BluetoothManager: NSObject, ObservableObject {
    @Published public var devices: [EyeballDevice] = []
    @Published public var isScanning: Bool = false
    @Published public var bluetoothState: CBManagerState = .unknown

    private var central: CBCentralManager!
    private var peripheralDelegates: [UUID: PeripheralDelegate] = [:]
    private let registry = DeviceRegistry()
    private var restoredKnownDevices = false
    // Devices the user disconnected on purpose — don't auto-reconnect those
    private var manualDisconnects: Set<UUID> = []

    private var rssiTimer: Timer?

    public override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
        // Poll RSSI on connected devices; advertisements cover the rest
        rssiTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            guard let self else { return }
            for dev in self.devices where dev.isConnected {
                dev.peripheral.readRSSI()
            }
        }
    }

    deinit {
        rssiTimer?.invalidate()
    }

    /// Recreate device entries for registry members and issue pending
    /// connects — CoreBluetooth completes them whenever the peripheral
    /// comes into range, which is what gives us auto-reconnect on launch.
    private func restoreKnownDevices() {
        guard !restoredKnownDevices else { return }
        restoredKnownDevices = true
        let ids = registry.known.map { $0.id }
        guard !ids.isEmpty else { return }
        for peripheral in central.retrievePeripherals(withIdentifiers: ids) {
            guard device(for: peripheral) == nil else { continue }
            let dev = EyeballDevice(peripheral: peripheral)
            if let saved = registry.known.first(where: { $0.id == peripheral.identifier }) {
                dev.name = saved.name
            }
            dev.isKnown = true
            devices.append(dev)
            central.connect(peripheral, options: nil)
        }
    }

    /// Drop a device from the persistent registry and the list.
    public func forget(_ device: EyeballDevice) {
        registry.remove(device.id)
        manualDisconnects.remove(device.id)
        central.cancelPeripheralConnection(device.peripheral)
        device.isKnown = false
        devices.removeAll { $0.id == device.id }
    }

    /// Registry bookkeeping when a device's name becomes known or changes.
    fileprivate func noteName(_ name: String, for device: EyeballDevice) {
        device.name = name
        if device.isKnown {
            registry.upsert(id: device.id, name: name)
        }
    }

    public func startScanning() {
        guard central.state == .poweredOn else { return }
        isScanning = true
        // Duplicates on: repeated advertisements keep RSSI live for
        // devices we haven't connected to yet (foreground scanning only)
        central.scanForPeripherals(withServices: [eyeballServiceUUID],
                                   options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
    }

    public func stopScanning() {
        central.stopScan()
        isScanning = false
    }

    public func connect(_ device: EyeballDevice) {
        manualDisconnects.remove(device.id)
        device.isConnecting = true
        central.connect(device.peripheral, options: nil)
    }

    public func disconnect(_ device: EyeballDevice) {
        manualDisconnects.insert(device.id)   // deliberate — suppress auto-reconnect
        central.cancelPeripheralConnection(device.peripheral)
    }

    public func write(data: Data, to entry: CharacteristicEntry, on device: EyeballDevice) {
        device.peripheral.writeValue(data, for: entry.characteristic, type: .withResponse)
    }

    public func writeFloat(_ value: Float, to entry: CharacteristicEntry, on device: EyeballDevice) {
        var v = value
        let data = Data(bytes: &v, count: 4)
        write(data: data, to: entry, on: device)
    }

    public func writeUInt8(_ value: UInt8, to entry: CharacteristicEntry, on device: EyeballDevice) {
        let data = Data([value])
        write(data: data, to: entry, on: device)
    }

    public func writeDeviceName(_ name: String, on device: EyeballDevice) {
        guard let entry = device.deviceNameEntry,
              let data = name.data(using: .utf8),
              data.count <= 20 else { return }
        write(data: data, to: entry, on: device)
        noteName(name, for: device)
    }

    private func device(for peripheral: CBPeripheral) -> EyeballDevice? {
        devices.first { $0.id == peripheral.identifier }
    }
}

// MARK: - CBCentralManagerDelegate
extension BluetoothManager: CBCentralManagerDelegate {
    public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        bluetoothState = central.state
        if central.state == .poweredOn {
            restoreKnownDevices()
            if isScanning { startScanning() }
        }
    }

    public func centralManager(_ central: CBCentralManager,
                               didDiscover peripheral: CBPeripheral,
                               advertisementData: [String: Any],
                               rssi RSSI: NSNumber) {
        if let existing = device(for: peripheral) {
            existing.updateRSSI(RSSI.intValue)
            return
        }
        let dev = EyeballDevice(peripheral: peripheral)
        if let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String {
            dev.name = name
        }
        dev.updateRSSI(RSSI.intValue)
        devices.append(dev)
    }

    public func centralManager(_ central: CBCentralManager,
                               didConnect peripheral: CBPeripheral) {
        guard let dev = device(for: peripheral) else { return }
        dev.isConnecting = false
        dev.isConnected = true
        dev.isKnown = true
        registry.upsert(id: dev.id, name: dev.name)

        let delegate = PeripheralDelegate(device: dev, manager: self)
        peripheralDelegates[peripheral.identifier] = delegate
        peripheral.delegate = delegate
        peripheral.discoverServices([eyeballServiceUUID])
    }

    public func centralManager(_ central: CBCentralManager,
                               didDisconnectPeripheral peripheral: CBPeripheral,
                               error: Error?) {
        guard let dev = device(for: peripheral) else { return }
        dev.isConnecting = false
        dev.isConnected = false
        dev.rssi = nil
        dev.characteristics.removeAll()
        peripheralDelegates.removeValue(forKey: peripheral.identifier)

        // Known device dropped without the user asking: issue a pending
        // connect so it re-attaches as soon as it's back in range
        if registry.contains(dev.id) && !manualDisconnects.contains(dev.id) {
            central.connect(peripheral, options: nil)
        }
    }
}

// MARK: - CBPeripheralDelegate (per-device)
private class PeripheralDelegate: NSObject, CBPeripheralDelegate {
    let device: EyeballDevice
    weak var manager: BluetoothManager?

    init(device: EyeballDevice, manager: BluetoothManager) {
        self.device = device
        self.manager = manager
    }

    func peripheral(_ peripheral: CBPeripheral, didReadRSSI RSSI: NSNumber, error: Error?) {
        guard error == nil else { return }
        let value = RSSI.intValue
        DispatchQueue.main.async { self.device.updateRSSI(value) }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == eyeballServiceUUID }) else { return }
        peripheral.discoverCharacteristics(nil, for: service)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        guard let chars = service.characteristics else { return }
        for chr in chars {
            peripheral.readValue(for: chr)
            if chr.properties.contains(.notify) {
                peripheral.setNotifyValue(true, for: chr)
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        DispatchQueue.main.async { [self] in
            if let existing = device.characteristics.first(where: { $0.id == characteristic.uuid }) {
                existing.value = characteristic.value ?? Data()
            } else {
                let label = Self.labelForUUID(characteristic.uuid)
                let entry = CharacteristicEntry(characteristic: characteristic, label: label)
                device.characteristics.append(entry)
                // Re-sort: device name first, then params, then stats
                device.characteristics.sort { $0.id.uuidString < $1.id.uuidString }
            }
            // The name characteristic is authoritative — advertisement
            // names can be stale
            if characteristic.uuid == deviceNameCharUUID,
               let data = characteristic.value,
               let name = String(data: data, encoding: .utf8), !name.isEmpty {
                manager?.noteName(name, for: device)
            }
        }
    }

    private static func labelForUUID(_ uuid: CBUUID) -> String {
        // Map known UUIDs to human-readable labels.
        // These match the firmware param names.
        switch uuid.uuidString {
        case "0001": return "Device Name"
        case "0010": return "Display Mode"
        case "0022": return "Battery V"
        case "0023": return "Battery %"
        case "0030": return "Firmware"
        default: return "Unknown (\(uuid.uuidString))"
        }
    }
}
