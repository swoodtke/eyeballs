# Eyeball — Waveshare ESP32-S3-Touch-LCD-1.46

## Hardware
- **Board**: Waveshare ESP32-S3-Touch-LCD-1.46 (ESP32-S3R8, 16 MB flash, 8 MB PSRAM)
- **Display**: SPD2010, 412×412 round AMOLED, **QSPI** (4 data lines)
- **IMU**: QMI8658 6-axis, I2C — built in, no wiring needed
- **IO expander**: TCA9554 (also I2C) — controls LCD reset pin

## What it does
| Gesture / Input | Effect |
|---|---|
| Tilt board | Pupil follows gravity |
| Jump / tap desk | Eye blinks (mic-triggered) |
| Swipe left/right | Switch display mode (next/previous) |
| Long-press (0.8s) | Toggle on-device status screen (name, battery, BLE, FPS) |
| Idle | Random saccade every 2–5 s |

### Eye modes
- **Cat Eye** — realistic eye with iris texture, sclera shading, and spring-physics pupil tracking
- **Hypnotoad** — rotating log-spiral with 4 configurable color bands

### BLE Controller App (macOS)
A SwiftUI app in `EyeballController/` connects to one or more eyeball devices via BLE and provides:
- Live stats (FPS, battery voltage/%, mic loudness)
- Mode-specific parameter controls (blink settings for cat eye, spiral zoom/speed/colors for hypnotoad)
- Device renaming (persisted to NVS flash)
- Mode switching that syncs bidirectionally with on-device touch

## Build & flash

### Firmware (ESP32)
```bash
. $IDF_PATH/export.sh          # ESP-IDF v5.x

idf.py set-target esp32s3

# First time only — fetches the SPD2010 driver from ESP Component Registry:
idf.py update-dependencies

idf.py build
idf.py -p /dev/cu.usbserial-* flash monitor
```

### Controller app (macOS)
```bash
cd EyeballController
swift build
swift run EyeballApp
```

## Key differences from the 1.28" GC9A01 version

| | 1.28" (GC9A01) | 1.46" (SPD2010) |
|---|---|---|
| Bus | 4-wire SPI | **QSPI (4 data lines)** |
| Resolution | 240×240 | **412×412** |
| LCD reset | Direct GPIO | Via **TCA9554** I2C expander |
| I2C pins | GPIO6/7 | **GPIO11/10** |
| Framebuffer | ~112 KB | **~330 KB** |
| LCD driver component | (manual) | `espressif/esp_lcd_spd2010` |

## Tuning knobs (top of main.c)
| Constant | Effect |
|---|---|
| `MAX_IRIS_TRAVEL` | Max pupil travel from centre (px) |
| `k = 14.0f` | Spring stiffness — higher = snappier tracking |
| `c = 6.0f` | Damping — higher = less bounce |
| `1.4f` (blink detect) | Z-accel delta threshold in g |
| `HALF = 0.175f` | Half-blink duration in seconds |
| `COL_IRIS` / `COL_IRIS_RIM` | Eye colour |

## Troubleshooting
- **Blank / white display**: Try halving the SPI clock (`40 * 1000 * 1000` → `20 * 1000 * 1000`) in `lcd_init()`
- **`idf_component.yml` error**: Run `idf.py update-dependencies` before build
- **IMU not found**: Check monitor — code auto-tries 0x6B then 0x6A; should log `WHO_AM_I=0x05`
- **PSRAM alloc failed**: Run menuconfig, enable SPIRAM, set to Octal 80 MHz for this board
