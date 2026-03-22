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

    public override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    public func startScanning() {
        guard central.state == .poweredOn else { return }
        isScanning = true
        central.scanForPeripherals(withServices: [eyeballServiceUUID],
                                   options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
    }

    public func stopScanning() {
        central.stopScan()
        isScanning = false
    }

    public func connect(_ device: EyeballDevice) {
        device.isConnecting = true
        central.connect(device.peripheral, options: nil)
    }

    public func disconnect(_ device: EyeballDevice) {
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
        device.name = name
    }

    private func device(for peripheral: CBPeripheral) -> EyeballDevice? {
        devices.first { $0.id == peripheral.identifier }
    }
}

// MARK: - CBCentralManagerDelegate
extension BluetoothManager: CBCentralManagerDelegate {
    public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        bluetoothState = central.state
        if central.state == .poweredOn && isScanning {
            startScanning()
        }
    }

    public func centralManager(_ central: CBCentralManager,
                               didDiscover peripheral: CBPeripheral,
                               advertisementData: [String: Any],
                               rssi RSSI: NSNumber) {
        guard device(for: peripheral) == nil else { return }
        let dev = EyeballDevice(peripheral: peripheral)
        if let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String {
            dev.name = name
        }
        devices.append(dev)
    }

    public func centralManager(_ central: CBCentralManager,
                               didConnect peripheral: CBPeripheral) {
        guard let dev = device(for: peripheral) else { return }
        dev.isConnecting = false
        dev.isConnected = true

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
        dev.characteristics.removeAll()
        peripheralDelegates.removeValue(forKey: peripheral.identifier)
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
        }
    }

    private static func labelForUUID(_ uuid: CBUUID) -> String {
        // Map known UUIDs to human-readable labels.
        // These match the firmware param names.
        switch uuid.uuidString {
        case "0001": return "Device Name"
        case "0010": return "Display Mode"
        case "0011": return "Blink Threshold"
        case "0012": return "Blink Close Speed"
        case "0013": return "Blink Open Speed"
        case "0014": return "Blink Hold Time"
        case "0020": return "Mic Loudness"
        case "0021": return "FPS"
        case "0022": return "Battery V"
        case "0023": return "Battery %"
        default: return "Unknown (\(uuid.uuidString))"
        }
    }
}
