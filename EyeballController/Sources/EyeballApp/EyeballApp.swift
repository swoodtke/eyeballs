import SwiftUI
import AppKit
import EyeballBLE
import EyeballUI

@main
struct EyeballApp: App {
    @StateObject private var bluetooth = BluetoothManager()

    init() {
        NSApplication.shared.setActivationPolicy(.regular)
    }

    var body: some Scene {
        WindowGroup {
            DeviceListView()
                .environmentObject(bluetooth)
                .frame(minWidth: 400, minHeight: 300)
                .onAppear {
                    NSApplication.shared.activate(ignoringOtherApps: true)
                }
                .onDisappear {
                    NSApplication.shared.terminate(nil)
                }
        }
    }
}
