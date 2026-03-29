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

    private static let catEyeUUIDs: Set<String> = ["0011", "0012", "0013", "0014"]
    private static let hypnotoadUUIDs: Set<String> = ["0015", "0016", "0017", "0018", "0019", "001A"]
    private static let blobEyeUUIDs: Set<String> = ["001B", "001C", "001D", "001E"]
    private static let globalUUIDs: Set<String> = ["0010"]  // Display Mode

    /// Parameters for the current display mode + global params.
    public var modeParameters: [CharacteristicEntry] {
        let mode = displayMode
        return parameters.filter { entry in
            let uuid = entry.id.uuidString
            if Self.globalUUIDs.contains(uuid) { return true }
            if uuid == "0001" { return false }  // device name handled separately
            switch mode {
            case 0: return Self.catEyeUUIDs.contains(uuid)
            case 1: return Self.hypnotoadUUIDs.contains(uuid)
            case 2: return Self.blobEyeUUIDs.contains(uuid)
            default: return true
            }
        }
    }

    /// Current display mode (0=Cat Eye, 1=Hypnotoad).
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
