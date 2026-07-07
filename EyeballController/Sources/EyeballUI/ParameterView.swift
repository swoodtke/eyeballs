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
        } else if entry.value.count == 3 && entry.isWritable {
            colorControl
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
            // Enum-style picker: vertical list so all names stay readable
            let current = Int(entry.uint8Value ?? 0)
            VStack(alignment: .leading, spacing: 8) {
                Text(entry.label)
                ForEach(Array(names.enumerated()), id: \.offset) { idx, name in
                    Button {
                        bluetooth.writeUInt8(UInt8(idx), to: entry, on: device)
                        entry.value = Data([UInt8(idx)])
                    } label: {
                        HStack {
                            Image(systemName: idx == current
                                  ? "largecircle.fill.circle" : "circle")
                                .foregroundStyle(idx == current ? Color.accentColor : .secondary)
                            Text(name)
                            Spacer()
                        }
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                }
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

    @ViewBuilder
    private var colorControl: some View {
        let color = Binding<Color>(
            get: {
                let d = entry.value
                guard d.count >= 3 else { return .white }
                return Color(
                    red: Double(d[0]) / 255.0,
                    green: Double(d[1]) / 255.0,
                    blue: Double(d[2]) / 255.0
                )
            },
            set: { newColor in
                #if canImport(AppKit)
                guard let components = NSColor(newColor).usingColorSpace(.sRGB) else { return }
                let red = components.redComponent
                let green = components.greenComponent
                let blue = components.blueComponent
                #else
                var red: CGFloat = 0, green: CGFloat = 0, blue: CGFloat = 0, alpha: CGFloat = 0
                UIColor(newColor).getRed(&red, green: &green, blue: &blue, alpha: &alpha)
                #endif
                let r = UInt8(clamping: Int(red * 255))
                let g = UInt8(clamping: Int(green * 255))
                let b = UInt8(clamping: Int(blue * 255))
                let data = Data([r, g, b])
                bluetooth.write(data: data, to: entry, on: device)
                entry.value = data
            }
        )
        HStack {
            Text(entry.label)
            Spacer()
            ColorPicker("", selection: color, supportsOpacity: false)
                .labelsHidden()
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
        case "0010": return ["Cat Eye", "Hypnotoad", "Sauron", "Spiral Rings", "Heart"]
        default: return nil
        }
    }

    private func sliderRange(for entry: CharacteristicEntry) -> (Double, Double) {
        // Add cases here as tunable params return to the firmware registry
        switch entry.id.uuidString {
        default: return (0.0, 100.0)
        }
    }
}
