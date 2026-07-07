# CLAUDE.md

## Project overview
Animated eyeball display on Waveshare round AMOLED boards (1.46" 412x412 and 1.75" 466x466). Five display modes: Cat Eye (realistic, IMU-tracked), Hypnotoad (rotating log-spiral), Sauron (fire + slit pupil), Spiral Rings, and Heart (the last three are pre-rendered animations from the eyedata flash partition). A SwiftUI app (macOS + iOS) controls devices over BLE.

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
idf.py -p /dev/tty.usbmodem101 flash
```
The environment already has ESP-IDF sourced — `idf.py` works directly, no need for `export.sh` (ESP-IDF lives at `~/Projects/esp/esp-idf` if you ever need it). The serial port matches `/dev/tty.usbmodem*` (not `/dev/cu.*`); find it with `ls /dev/tty.usbmodem*` since the number can change. Do not run `idf.py monitor` — it's interactive and will hang the shell.

### macOS app
```bash
cd EyeballController && swift build
```

## Workflow
- **Build and flash firmware directly.** Always run `idf.py build` after firmware changes, then flash with `idf.py -p /dev/tty.usbmodem* flash`. If flashing fails, the board may be disconnected — tell the user rather than retrying. Never run monitor commands.
- **Rebuild firmware when changing C files.** The app only needs rebuilding for Swift changes.
- **CoreBluetooth caches aggressively.** If new BLE characteristics don't appear after flashing, the user may need to toggle Bluetooth off/on in System Settings.

## BLE param system
Parameters are registered in `main.c` as a `ble_param_t` array passed to `ble_init()`. Each param has a 16-bit UUID, flags (RW, RWN, or STAT), and a pointer to the actual variable. The registry is deliberately minimal (Display Mode + battery stats) — internal tuning variables exist in firmware but are only exposed over BLE once proven useful. Adding a new param:
1. Add the variable in `main.c`
2. Add an entry to `ble_params[]` with a unique UUID
3. Add the UUID to `labelForUUID()` in `BluetoothManager.swift` (in `Sources/EyeballBLE`)
4. If it needs a custom slider range, add it to `sliderRange()` in `ParameterView.swift` (in `Sources/EyeballUI`)

Flag types: `BLE_PARAM_RW` (read+write), `BLE_PARAM_RWN` (read+write+notify — use when device can change the value locally), `BLE_PARAM_STAT` (read+notify, for stats pushed to app).

## Commit conventions
- No `Co-Authored-By` lines in commit messages
- Commit style: short summary line, blank line, description paragraph
