import SwiftUI
import EyeballBLE

struct DeviceListView: View {
    @EnvironmentObject var bluetooth: BluetoothManager
    @State private var selectedDevice: EyeballDevice?

    var body: some View {
        NavigationSplitView {
            List(bluetooth.devices, selection: $selectedDevice) { device in
                DeviceRow(device: device, bluetooth: bluetooth, selectedDevice: $selectedDevice)
                    .tag(device)
            }
            .navigationTitle("Eyeballs")
            .navigationSplitViewColumnWidth(min: 220, ideal: 250)
            .toolbar {
                ToolbarItem {
                    Button(bluetooth.isScanning ? "Stop" : "Scan") {
                        if bluetooth.isScanning {
                            bluetooth.stopScanning()
                        } else {
                            bluetooth.startScanning()
                        }
                    }
                }
            }
            .onAppear {
                bluetooth.startScanning()
            }
        } detail: {
            if let device = selectedDevice {
                DeviceDetailView(device: device)
                    .environmentObject(bluetooth)
            } else {
                Text("Select a device")
                    .foregroundStyle(.secondary)
            }
        }
    }
}

private struct DeviceDetailView: View {
    @ObservedObject var device: EyeballDevice
    @EnvironmentObject var bluetooth: BluetoothManager

    var body: some View {
        if device.isConnected {
            DeviceDashboardView(device: device)
                .environmentObject(bluetooth)
        } else if device.isConnecting {
            VStack(spacing: 12) {
                ProgressView()
                Text("Connecting to \(device.name)...")
                    .foregroundStyle(.secondary)
            }
        } else {
            VStack(spacing: 12) {
                Text(device.name).font(.title2)
                Text("Not connected")
                    .foregroundStyle(.secondary)
                Button("Connect") {
                    bluetooth.connect(device)
                }
            }
        }
    }
}

private struct DeviceRow: View {
    @ObservedObject var device: EyeballDevice
    let bluetooth: BluetoothManager
    @Binding var selectedDevice: EyeballDevice?

    var body: some View {
        HStack {
            Circle()
                .fill(device.isConnected ? .green : (device.isConnecting ? .orange : .gray))
                .frame(width: 8, height: 8)
            Text(device.name)
            Spacer()
            Button(device.isConnected ? "Disconnect" : "Connect") {
                if device.isConnected {
                    bluetooth.disconnect(device)
                } else {
                    selectedDevice = device
                    bluetooth.connect(device)
                }
            }
            .buttonStyle(.borderless)
        }
    }
}
