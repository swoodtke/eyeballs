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
            }

            if !device.modeParameters.isEmpty {
                Section("Parameters") {
                    ForEach(device.modeParameters) { entry in
                        ParameterView(entry: entry, device: device)
                            .environmentObject(bluetooth)
                    }
                }
            }

            if !device.stats.isEmpty {
                Section("Stats") {
                    ForEach(device.stats) { entry in
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

    private func logBattery() {
        let voltage = device.characteristics.first { $0.label == "Battery V" }?.floatValue ?? 0
        let percent = device.characteristics.first { $0.label == "Battery %" }?.floatValue ?? 0
        let raw = device.characteristics.first { $0.label == "BAT ADC Raw" }?.floatValue ?? 0
        let ts = ISO8601DateFormatter().string(from: Date())
        print("\(ts) BAT: \(String(format: "%.2fV", voltage)) \(String(format: "%.0f%%", percent)) raw=\(String(format: "%.0f", raw))")
    }

    private func saveName() {
        let trimmed = String(newName.prefix(20))
        guard !trimmed.isEmpty else { return }
        bluetooth.writeDeviceName(trimmed, on: device)
        editingName = false
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
