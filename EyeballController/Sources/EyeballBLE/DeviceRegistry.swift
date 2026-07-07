import Foundation

/// A device we've successfully connected to before, persisted across launches.
public struct KnownDevice: Codable, Equatable, Identifiable {
    public let id: UUID       // CBPeripheral.identifier (stable per host device)
    public var name: String
}

/// Persists known devices in UserDefaults so the app can list them on launch
/// and reconnect automatically when they come back into range.
final class DeviceRegistry {
    private static let defaultsKey = "knownEyeballDevices"
    private(set) var known: [KnownDevice]

    init() {
        if let data = UserDefaults.standard.data(forKey: Self.defaultsKey),
           let decoded = try? JSONDecoder().decode([KnownDevice].self, from: data) {
            known = decoded
        } else {
            known = []
        }
    }

    func contains(_ id: UUID) -> Bool {
        known.contains { $0.id == id }
    }

    func upsert(id: UUID, name: String) {
        if let idx = known.firstIndex(where: { $0.id == id }) {
            guard known[idx].name != name else { return }
            known[idx].name = name
        } else {
            known.append(KnownDevice(id: id, name: name))
        }
        save()
    }

    func remove(_ id: UUID) {
        known.removeAll { $0.id == id }
        save()
    }

    private func save() {
        if let data = try? JSONEncoder().encode(known) {
            UserDefaults.standard.set(data, forKey: Self.defaultsKey)
        }
    }
}
