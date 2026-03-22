import SwiftUI
import CoreBluetooth
import EyeballBLE

struct ParameterView: View {
    @ObservedObject var entry: CharacteristicEntry
    @ObservedObject var device: EyeballDevice
    @EnvironmentObject var bluetooth: BluetoothManager

    var body: some View {
        // Device name is handled separately in the dashboard
        if entry.id == CBUUID(string: "0001") {
            EmptyView()
        } else if entry.value.count == 1 {
            uint8Control
        } else if entry.value.count == 4 {
            floatControl
        } else {
            rawView
        }
    }

    @ViewBuilder
    private var uint8Control: some View {
        let modeNames = displayModeNames(for: entry)
        if let names = modeNames {
            // Enum-style picker
            HStack {
                Text(entry.label)
                Spacer()
                Picker("", selection: Binding(
                    get: { Int(entry.uint8Value ?? 0) },
                    set: { newVal in
                        bluetooth.writeUInt8(UInt8(newVal), to: entry, on: device)
                        entry.value = Data([UInt8(newVal)])
                    }
                )) {
                    ForEach(Array(names.enumerated()), id: \.offset) { idx, name in
                        Text(name).tag(idx)
                    }
                }
                .pickerStyle(.segmented)
                .frame(maxWidth: 200)
            }
        } else {
            // Generic uint8 stepper
            HStack {
                Text(entry.label)
                Spacer()
                Text("\(entry.uint8Value ?? 0)")
                    .monospacedDigit()
                Stepper("", value: Binding(
                    get: { Int(entry.uint8Value ?? 0) },
                    set: { newVal in
                        let clamped = UInt8(clamping: newVal)
                        bluetooth.writeUInt8(clamped, to: entry, on: device)
                        entry.value = Data([clamped])
                    }
                ), in: 0...255)
                .labelsHidden()
            }
        }
    }

    @ViewBuilder
    private var floatControl: some View {
        let range = sliderRange(for: entry)
        HStack {
            Text(entry.label)
            Spacer()
            Text(String(format: "%.2f", entry.floatValue ?? 0))
                .monospacedDigit()
                .frame(width: 50, alignment: .trailing)
            Slider(value: Binding(
                get: { Double(entry.floatValue ?? 0) },
                set: { newVal in
                    let f = Float(newVal)
                    bluetooth.writeFloat(f, to: entry, on: device)
                    var v = f
                    entry.value = Data(bytes: &v, count: 4)
                }
            ), in: range.0...range.1)
            .frame(maxWidth: 200)
        }
    }

    private var rawView: some View {
        HStack {
            Text(entry.label)
            Spacer()
            Text(entry.value.map { String(format: "%02x", $0) }.joined())
                .foregroundStyle(.secondary)
        }
    }

    private func displayModeNames(for entry: CharacteristicEntry) -> [String]? {
        switch entry.id.uuidString {
        case "0010": return ["Cat Eye", "Hypnotoad"]
        default: return nil
        }
    }

    private func sliderRange(for entry: CharacteristicEntry) -> (Double, Double) {
        switch entry.id.uuidString {
        case "0011": return (0.0, 1.0)      // Blink threshold
        case "0012": return (1.0, 20.0)     // Close speed
        case "0013": return (0.5, 10.0)     // Open speed
        case "0014": return (0.01, 0.5)     // Hold time
        default: return (0.0, 100.0)
        }
    }
}
