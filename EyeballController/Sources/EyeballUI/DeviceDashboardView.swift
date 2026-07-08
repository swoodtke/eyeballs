import SwiftUI
import Foundation
import EyeballBLE

struct DeviceDashboardView: View {
    @ObservedObject var device: EyeballDevice
    @EnvironmentObject var bluetooth: BluetoothManager
    @State private var editingName = false
    @State private var newName = ""
    @FocusState private var nameFieldFocused: Bool
    @State private var logTimer: Timer?

    var body: some View {
        Form {
            Section("Device") {
                HStack {
                    if editingName {
                        TextField("Name", text: $newName)
                            .textFieldStyle(.roundedBorder)
                            .focused($nameFieldFocused)
                            .onSubmit { saveName() }
                        Button("Save") { saveName() }
                        Button("Cancel") {
                            editingName = false
                            nameFieldFocused = false
                        }
                    } else {
                        Text(device.name).font(.headline)
                        Spacer()
                        Button("Rename") {
                            newName = device.name
                            editingName = true
                            // Delay focus to next runloop so the TextField exists
                            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                                nameFieldFocused = true
                            }
                        }
                    }
                }

                HStack {
                    Text("Signal")
                    Spacer()
                    SignalCirclesView(bars: device.signalBars)
                    Text(device.rssi.map { "\($0) dBm" } ?? "—")
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                        .frame(width: IndicatorLayout.valueWidth, alignment: .trailing)
                }

                if let percent = batteryPercentEntry, let voltage = batteryVoltageEntry {
                    BatteryRow(percent: percent, voltage: voltage)
                }

                if let fw = device.firmwareVersion, !fw.isEmpty {
                    HStack {
                        Text("Firmware")
                        Spacer()
                        Text(fw)
                            .monospaced()
                            .foregroundStyle(.secondary)
                    }
                }

                if let rotation = device.rotationEntry {
                    RotationRow(entry: rotation, device: device)
                        .environmentObject(bluetooth)
                }
            }

            if let mode = device.displayModeEntry {
                Section("Display Mode") {
                    ModeListView(entry: mode, device: device)
                        .environmentObject(bluetooth)
                }
            }

            Section("Mode Options") {
                if device.variantParameters.isEmpty {
                    Text("No options for this mode")
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(device.variantParameters) { entry in
                        ParameterView(entry: entry, device: device)
                            .environmentObject(bluetooth)
                    }
                }
            }

            // Stats other than battery (which lives in the Device section)
            let extraStats = device.stats.filter {
                !["0022", "0023"].contains($0.id.uuidString)
            }
            if !extraStats.isEmpty {
                Section("Stats") {
                    ForEach(extraStats) { entry in
                        StatView(entry: entry)
                    }
                }
            }
        }
        .formStyle(.grouped)
        .navigationTitle(device.name)
        .onAppear { startBatteryLog() }
        .onDisappear { logTimer?.invalidate(); logTimer = nil }
    }

    private func startBatteryLog() {
        logBattery()
        logTimer = Timer.scheduledTimer(withTimeInterval: 60, repeats: true) { _ in
            logBattery()
        }
    }

    private var batteryVoltageEntry: CharacteristicEntry? {
        device.characteristics.first { $0.id.uuidString == "0022" }
    }

    private var batteryPercentEntry: CharacteristicEntry? {
        device.characteristics.first { $0.id.uuidString == "0023" }
    }

    private func logBattery() {
        let voltage = batteryVoltageEntry?.floatValue ?? 0
        let percent = batteryPercentEntry?.floatValue ?? 0
        let ts = ISO8601DateFormatter().string(from: Date())
        print("\(ts) BAT: \(String(format: "%.2fV", voltage)) \(String(format: "%.0f%%", percent))")
    }

    private func saveName() {
        // The device stores at most 20 UTF-8 bytes, not 20 characters —
        // trim whole characters until the encoded name fits
        var trimmed = newName
        while trimmed.utf8.count > 20 && !trimmed.isEmpty {
            trimmed.removeLast()
        }
        guard !trimmed.isEmpty else { return }
        bluetooth.writeDeviceName(trimmed, on: device)
        editingName = false
    }
}

/// Display rotation for the mounting orientation (goggle vs pendant).
private struct RotationRow: View {
    @ObservedObject var entry: CharacteristicEntry
    @ObservedObject var device: EyeballDevice
    @EnvironmentObject var bluetooth: BluetoothManager

    var body: some View {
        HStack {
            Text("Rotation")
            Spacer()
            Picker("", selection: Binding(
                get: { Int(entry.uint8Value ?? 0) },
                set: { newVal in
                    bluetooth.writeUInt8(UInt8(newVal), to: entry, on: device)
                    entry.value = Data([UInt8(newVal)])
                }
            )) {
                Text("0°").tag(0)
                Text("90°").tag(1)
                Text("180°").tag(2)
                Text("270°").tag(3)
            }
            .pickerStyle(.segmented)
            .frame(maxWidth: 220)
        }
    }
}

/// Battery percentage and voltage combined into one row.
private struct BatteryRow: View {
    @ObservedObject var percent: CharacteristicEntry
    @ObservedObject var voltage: CharacteristicEntry

    var body: some View {
        HStack {
            Text("Battery")
            Spacer()
            BatteryCirclesView(percent: Double(percent.floatValue ?? 0))
            Text(voltage.floatValue.map { String(format: "%.2f V", $0) } ?? "—")
                .monospacedDigit()
                .foregroundStyle(.secondary)
                .frame(width: IndicatorLayout.valueWidth, alignment: .trailing)
        }
    }
}

private struct StatView: View {
    @ObservedObject var entry: CharacteristicEntry

    var body: some View {
        HStack {
            Text(entry.label)
            Spacer()
            if let f = entry.floatValue {
                Text(formatStat(label: entry.label, value: f))
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
            } else {
                Text(entry.value.map { String(format: "%02x", $0) }.joined())
                    .foregroundStyle(.secondary)
            }
        }
    }

    private func formatStat(label: String, value: Float) -> String {
        switch label {
        case "Battery %": return String(format: "%.0f%%", value)
        case "Battery V": return String(format: "%.2fV", value)
        case "FPS":         return String(format: "%.1f", value)
        case "BAT ADC Raw": return String(format: "%.0f", value)
        default:          return String(format: "%.2f", value)
        }
    }
}
