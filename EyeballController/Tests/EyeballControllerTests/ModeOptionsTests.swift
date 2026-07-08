import XCTest
import CoreBluetooth
@testable import EyeballBLE
@testable import EyeballUI

/// View-model-level tests for the Mode Options section — no radio needed:
/// CBMutableCharacteristic instances stand in for discovered characteristics.
final class ModeOptionsTests: XCTestCase {

    private func entry(_ uuid: String,
                       properties: CBCharacteristicProperties = [.read, .write, .notify])
        -> CharacteristicEntry
    {
        let chr = CBMutableCharacteristic(
            type: CBUUID(string: uuid),
            properties: properties,
            value: nil,
            permissions: [.readable, .writeable])
        return CharacteristicEntry(characteristic: chr, label: uuid)
    }

    /// A discovered characteristic set matching the current firmware registry.
    private var firmwareEntries: [CharacteristicEntry] {
        [
            entry("0001"),                                  // Device Name
            entry("0010"),                                  // Display Mode
            entry("0022", properties: [.read, .notify]),    // Battery V
            entry("0023", properties: [.read, .notify]),    // Battery %
            entry("0025"),                                  // Brightness
            entry("0030", properties: [.read]),             // FW Version
            entry("0031"),                                  // Rotation
            entry("0032"),                                  // Sync Group
            entry("0033"),                                  // Sync Role
            entry("0034", properties: [.read, .write]),     // Clock
            entry("0035", properties: [.read, .write]),     // Power Off
            entry("0036"),                                  // Glow Color
            entry("0040"),                                  // Sync Data
        ]
    }

    func testGlowColorShownInGlowAndBallModes() {
        for mode in [UInt8(6), UInt8(7)] {   // Glow, Ball
            let options = EyeballDevice.variantParameters(in: firmwareEntries,
                                                          forMode: mode)
            XCTAssertEqual(options.map { $0.id.uuidString }, ["0036"],
                           "Mode \(mode) should offer exactly the Glow Color option")
        }
    }

    func testGlowColorHiddenInOtherModes() {
        for mode in UInt8(0)...5 {
            let options = EyeballDevice.variantParameters(in: firmwareEntries,
                                                          forMode: mode)
            XCTAssertTrue(options.isEmpty,
                          "Mode \(mode) should have no options, got " +
                          "\(options.map { $0.id.uuidString })")
        }
    }

    func testNonWritableEntriesNeverAppear() {
        let stats = [entry("0022", properties: [.read, .notify]),
                     entry("0030", properties: [.read])]
        let options = EyeballDevice.variantParameters(in: stats, forMode: 6)
        XCTAssertTrue(options.isEmpty)
    }

    /// The app's mode list order must match the firmware's display_mode_t,
    /// and the per-mode param map must agree with that order.
    func testModeListMatchesParameterModeMap() {
        XCTAssertEqual(ModeListView.modeNames.count, 8,
                       "modeNames must match the firmware's NUM_MODES")
        XCTAssertEqual(ModeListView.modeNames.firstIndex(of: "Glow"), 6)
        XCTAssertEqual(ModeListView.modeNames.firstIndex(of: "Ball"), 7)
        XCTAssertEqual(EyeballDevice.parameterModes["0036"], [6, 7],
                       "Glow Color must be mapped to the Glow and Ball modes")
    }
}
