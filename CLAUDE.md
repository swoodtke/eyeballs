# CLAUDE.md

## Project overview
Animated eyeball display on a Waveshare ESP32-S3-Touch-LCD-1.46 (412x412 round AMOLED). Two eye modes: Cat Eye (realistic, IMU-tracked) and Hypnotoad (rotating log-spiral). A macOS SwiftUI app controls devices over BLE.

## Repo structure
- `main/` — ESP32 firmware (C, ESP-IDF)
  - `main.c` — rendering, IMU, eye physics, BLE param registration
  - `ble.c` / `ble.h` — NimBLE GATT server, param registry, device naming
- `EyeballController/` — macOS SwiftUI app (Swift Package Manager)
  - `Sources/EyeballBLE/` — BLE library (BluetoothManager, EyeballDevice, CharacteristicEntry)
  - `Sources/EyeballApp/` — SwiftUI views (DeviceDashboardView, ParameterView)

## Build commands

### Firmware
```bash
idf.py build
```
ESP-IDF must be sourced first (`. $IDF_PATH/export.sh`). The user handles flashing — do not run `idf.py flash`.

### macOS app
```bash
cd EyeballController && swift build
```

## Workflow
- **I build firmware, user flashes.** Always run `idf.py build` after firmware changes. Never run flash/monitor commands.
- **Rebuild firmware when changing C files.** The app only needs rebuilding for Swift changes.
- **CoreBluetooth caches aggressively.** If new BLE characteristics don't appear after flashing, the user may need to toggle Bluetooth off/on in System Settings.

## BLE param system
Parameters are registered in `main.c` as a `ble_param_t` array passed to `ble_init()`. Each param has a 16-bit UUID, flags (RW, RWN, or STAT), and a pointer to the actual variable. Adding a new param:
1. Add the variable in `main.c`
2. Add an entry to `ble_params[]` with a unique UUID
3. Add the UUID to `labelForUUID()` in `BluetoothManager.swift`
4. Add the UUID to the appropriate mode set in `EyeballDevice.swift` (catEyeUUIDs, hypnotoadUUIDs, or globalUUIDs)
5. If it needs a custom slider range, add it to `sliderRange()` in `ParameterView.swift`

Flag types: `BLE_PARAM_RW` (read+write), `BLE_PARAM_RWN` (read+write+notify — use when device can change the value locally), `BLE_PARAM_STAT` (read+notify, for stats pushed to app).

## Commit conventions
- No `Co-Authored-By` lines in commit messages
- Commit style: short summary line, blank line, description paragraph
