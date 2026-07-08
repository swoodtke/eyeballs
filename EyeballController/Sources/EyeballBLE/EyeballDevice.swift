import Combine
import CoreBluetooth
import Foundation

/// A single discovered characteristic with its current value.
public class CharacteristicEntry: ObservableObject, Identifiable {
    public let id: CBUUID
    public let characteristic: CBCharacteristic
    public let label: String
    public let isWritable: Bool
    public let isNotifiable: Bool

    @Published public var value: Data

    init(characteristic: CBCharacteristic, label: String) {
        self.id = characteristic.uuid
        self.characteristic = characteristic
        self.label = label
        self.isWritable = characteristic.properties.contains(.write)
        self.isNotifiable = characteristic.properties.contains(.notify)
        self.value = characteristic.value ?? Data()
    }

    /// Interpret the value as a little-endian Float (4 bytes).
    public var floatValue: Float? {
        guard value.count == 4 else { return nil }
        // Data slices aren't guaranteed 4-byte aligned; load(as:) can trap
        return value.withUnsafeBytes { $0.loadUnaligned(as: Float.self) }
    }

    /// Interpret the value as a single UInt8 (1 byte).
    public var uint8Value: UInt8? {
        guard value.count == 1 else { return nil }
        return value[0]
    }

    /// Interpret the value as a UTF-8 string (fixed-size characteristics
    /// arrive NUL-padded, so decode up to the first NUL).
    public var stringValue: String? {
        String(data: value.prefix(while: { $0 != 0 }), encoding: .utf8)
    }
}

/// Represents one connected eyeball device with its discovered characteristics.
public class EyeballDevice: ObservableObject, Identifiable, Hashable {
    public static func == (lhs: EyeballDevice, rhs: EyeballDevice) -> Bool { lhs.id == rhs.id }
    public func hash(into hasher: inout Hasher) { hasher.combine(id) }
    public let id: UUID
    public let peripheral: CBPeripheral
    @Published public var name: String
    @Published public var isConnecting: Bool = false
    @Published public var isConnected: Bool = false
    /// True once the device is in the persistent registry (connected at
    /// least once); known devices auto-reconnect when back in range.
    @Published public var isKnown: Bool = false
    /// Smoothed signal strength in dBm; nil until a reading arrives (or
    /// after disconnect). Raw RSSI is noisy, so readings are blended.
    @Published public var rssi: Int?

    public func updateRSSI(_ raw: Int) {
        guard raw < 0, raw > -127 else { return }   // 127 = "unavailable"
        if let current = rssi {
            rssi = Int((Double(current) * 0.7 + Double(raw) * 0.3).rounded())
        } else {
            rssi = raw
        }
    }

    /// Proximity as 1–3 bars (3 = close, 2 = near, 1 = far); 0 = unknown.
    /// Three buckets ~15 dB apart stay stable against RSSI noise — finer
    /// steps would flicker.
    public var signalBars: Int {
        guard let r = rssi else { return 0 }
        if r >= -55 { return 3 }
        if r >= -70 { return 2 }
        return 1
    }
    @Published public var characteristics: [CharacteristicEntry] = [] {
        didSet { subscribeToCharacteristics() }
    }
    private var characteristicSubs: Set<AnyCancellable> = []

    public init(peripheral: CBPeripheral) {
        self.id = peripheral.identifier
        self.peripheral = peripheral
        self.name = peripheral.name ?? "Unknown"
    }

    /// Stats = read + notify (no write).
    public var stats: [CharacteristicEntry] {
        characteristics.filter { $0.isNotifiable && !$0.isWritable }
    }

    /// All writable parameters.
    public var parameters: [CharacteristicEntry] {
        characteristics.filter { $0.isWritable }
    }

    /// The Display Mode characteristic (generic device control).
    public var displayModeEntry: CharacteristicEntry? {
        characteristics.first { $0.id.uuidString == "0010" }
    }

    /// Firmware version string reported by the device (git describe).
    public var firmwareVersion: String? {
        characteristics.first { $0.id.uuidString == "0030" }?.stringValue
    }

    /// The display rotation characteristic (0-3 = 0/90/180/270 degrees).
    public var rotationEntry: CharacteristicEntry? {
        characteristics.first { $0.id.uuidString == "0031" }
    }

    /// Per-variant tunables — everything writable that isn't a generic
    /// device control (name, display mode, rotation). Empty until the
    /// firmware re-exposes mode-specific params.
    public var variantParameters: [CharacteristicEntry] {
        parameters.filter { !["0001", "0010", "0031"].contains($0.id.uuidString) }
    }

    /// Current display mode (0=Cat Eye, 1=Hypnotoad, 2=Sauron,
    /// 3=Spiral Rings, 4=Heart).
    public var displayMode: UInt8 {
        characteristics.first { $0.id == CBUUID(string: "0010") }?.uint8Value ?? 0
    }

    /// The device name characteristic (UUID 0x0001).
    public var deviceNameEntry: CharacteristicEntry? {
        characteristics.first { $0.id == CBUUID(string: "0001") }
    }

    private func subscribeToCharacteristics() {
        characteristicSubs.removeAll()
        for entry in characteristics {
            entry.$value
                .receive(on: RunLoop.main)
                .sink { [weak self] _ in self?.objectWillChange.send() }
                .store(in: &characteristicSubs)
        }
    }
}
