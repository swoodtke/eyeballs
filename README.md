# Eyeball — Waveshare ESP32-S3-Touch-LCD-1.46

## Hardware
- **Board**: Waveshare ESP32-S3-Touch-LCD-1.46 (ESP32-S3R8, 16 MB flash, 8 MB PSRAM)
- **Display**: SPD2010, 412×412 round AMOLED, **QSPI** (4 data lines)
- **IMU**: QMI8658 6-axis, I2C — built in, no wiring needed
- **IO expander**: TCA9554 (also I2C) — controls LCD reset pin

## What it does
| Gesture | Eye |
|---|---|
| Tilt board | Pupil follows gravity |
| Jump / tap desk | Eye blinks |
| Idle | Random saccade every 2–5 s |

## Build & flash

```bash
. $IDF_PATH/export.sh          # ESP-IDF v5.1+

cd eyeball
idf.py set-target esp32s3

# This fetches the SPD2010 driver from the ESP Component Registry:
idf.py update-dependencies

idf.py menuconfig
# Verify: Component config → ESP PSRAM → Support for external SPI RAM = ON
#         Set to Octal mode, 80 MHz

idf.py build
idf.py -p /dev/cu.usbserial-* flash monitor
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
