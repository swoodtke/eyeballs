//
//  Eyeball_ControllerApp.swift
//  Eyeball Controller
//
//  Created by Shawn Woodtke on 2026-07-07.
//

import SwiftUI
import EyeballBLE
import EyeballUI

@main
struct EyeballPhoneApp: App {
    @StateObject private var bluetooth = BluetoothManager()

    var body: some Scene {
        WindowGroup {
            DeviceListView()
                .environmentObject(bluetooth)
        }
    }
}
