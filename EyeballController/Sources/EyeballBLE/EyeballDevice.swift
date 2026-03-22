import CoreBluetooth
import Foundation

/// A single discovered characteristic with its current value.
public class CharacteristicEntry: ObservableObject, Identifiable {
    public let id: CBUUID
    public let characteristic: CBCharacteristic
    public let label: String
    public let isWritable: Bool
    public let isNotifiable: Bool
    public let dataLength: Int

    @Published public var value: Data

    init(characteristic: CBCharacteristic, label: String) {
        self.id = characteristic.uuid
        self.characteristic = characteristic
        self.label = label
        self.isWritable = characteristic.properties.contains(.write)
        self.isNotifiable = characteristic.properties.contains(.notify)
        self.dataLength = characteristic.value?.count ?? 0
        self.value = characteristic.value ?? Data()
    }

    /// Interpret the value as a little-endian Float (4 bytes).
    public var floatValue: Float? {
        guard value.count == 4 else { return nil }
        return value.withUnsafeBytes { $0.load(as: Float.self) }
    }

    /// Interpret the value as a single UInt8 (1 byte).
    public var uint8Value: UInt8? {
        guard value.count == 1 else { return nil }
        return value[0]
    }

    /// Interpret the value as a UTF-8 string.
    public var stringValue: String? {
        String(data: value, encoding: .utf8)
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
    @Published public var characteristics: [CharacteristicEntry] = []

    public init(peripheral: CBPeripheral) {
        self.id = peripheral.identifier
        self.peripheral = peripheral
        self.name = peripheral.name ?? "Unknown"
    }

    /// Stats = read + notify (no write).
    public var stats: [CharacteristicEntry] {
        characteristics.filter { $0.isNotifiable && !$0.isWritable }
    }

    /// Parameters = read + write (no notify).
    public var parameters: [CharacteristicEntry] {
        characteristics.filter { $0.isWritable }
    }

    /// The device name characteristic (UUID 0x0001).
    public var deviceNameEntry: CharacteristicEntry? {
        characteristics.first { $0.id == CBUUID(string: "0001") }
    }
}
