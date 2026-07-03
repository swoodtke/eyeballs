# TODO

## Code review findings (2026-07-02)

### Must fix
- [ ] BLE Display Mode write crash — `main/main.c:2195` casts `ble_display_mode` to `display_mode_t` with no range check. A write ≥ 4 falls through both switches in `eye_mode_switch()`: `cat_eye_free()` runs (`iris_tex = NULL`) but nothing inits, and the next frame `eye_draw_cat()` dereferences NULL `iris_tex` (`main.c:1336`). Clamp like the touch handler does (`% NUM_MODES` at `main.c:2138`).
- [ ] BLE Brightness param silently missing — `ble_params[]` has 26 entries but `MAX_PARAMS` is 24 (`main/ble.c:31`); `ble_init()` truncates, so BAT ADC Raw (0x0024) and Brightness (0x0025) never enter the GATT table. Raise `MAX_PARAMS` (also sizes `notify_handles`, `chr_defs`, `chr_uuids`) and add a `_Static_assert`/`ESP_LOGE` on overflow.
- [ ] Stack overflow on malformed animation data — `main/main.c:1567–1577` reads `pal_size` from the eyedata partition header unchecked; `esp_partition_read` writes `pal_size * 3` bytes into a 192-byte stack buffer, then the copy loop overflows `anim_palette_rgb565[64]`. Reject `pal_size > 64`.

### Should fix
- [ ] 1.75" last flush strip exceeds `max_transfer_sz` — floor division at `main.c:383` and `main.c:658` gives 116-row strips but the last strip is 118 rows (110,024 bytes vs 108,112 limit). Use ceiling division in both places.
- [ ] `mic_sensitivity` ≤ 1.0 breaks audio reactivity — `main.c:1017` divides by `(mic_sensitivity - 1.0f)`: div-by-zero at 1.0, negative below, drives `mic_loudness` negative/NaN. Clamp the value or the result.
- [ ] PSRAM leak in `spiral_lut_init` — `main.c:1400`: if the 2nd/3rd alloc fails, earlier buffers leak; repeated mode switches compound it. Free non-NULL pointers on failure (as `anim_init` does).
- [ ] Last animation frame decoded with wrong compressed size — `main.c:1613` falls back to uncompressed size for the last frame, so the RLE decoder reads garbage past the real data. Bound with the partition's `anim_size`.
- [ ] Device rename silently fails on multi-byte characters — app truncates to 20 *characters* (`BluetoothManager.swift:62`) but the write guard checks 20 *UTF-8 bytes* (`DeviceDashboardView.swift:81`), so emoji/accented names silently revert. Enforce the limit in bytes.
- [ ] `logBattery()` matches characteristics by label string (`DeviceDashboardView.swift:72`) — renaming "Battery V" in `labelForUUID` silently breaks logging. Match on UUID.

### Repo hygiene
- [ ] `git rm -r mnt/` — `mnt/user-data/outputs/eyeball_1_46/main/CMakeLists.txt` is a stale duplicate of `main/CMakeLists.txt`
- [ ] `git rm EyeballController/build.log` and gitignore it — tracked build artifact

### Minor
- [ ] No BLE pairing/bonding — anyone in range can rename/control the device (and trigger the mode-write crash). Consider `BLE_GATT_CHR_F_WRITE_ENC` + bonding, or accept as a conscious choice.
- [ ] `EyeballDevice.swift:29` uses `load(as: Float.self)` on `Data` — can trap on unaligned memory; use `loadUnaligned(as:)`.
- [ ] BLE writes hit globals with no sync against the render loop — mostly cosmetic (one garbled color frame), but a BLE mode write can discard a simultaneous touch mode change.
- [ ] Dead code: `draw_cat_pupil` (`main.c:1164`) never called; `te_sem` given from ISR but never taken; `CharacteristicEntry.dataLength` frozen at init and unused.
- [ ] `EyeballDevice.swift:96` doc comment lists only 2 of 4 display modes.
- [ ] `subscribeToCharacteristics()` rebuilds all Combine subscriptions on every characteristic append — O(N²) churn during discovery, harmless at N=26.
- [ ] `board_config.h:26` unconditionally includes legacy `driver/i2c.h` even when the new I2C master API is in use — potential symbol conflicts.

## Performance
- [x] Use both ESP32-S3 cores — rendering on Core 1 with double-buffered framebuffers, sensors/physics on Core 0. ~29→39 FPS.
- [ ] Skip rendering corner pixels outside the round display — precompute per-row x-bounds for the display circle (like sclera_x0/x1 but for full display diameter), only iterate within those bounds, memset the rest to black. Currently ~21.5% of pixels are outside the visible circle but still get per-pixel work (circle tests, mask lookups). Cat eye already does cheap black fill so gains are mainly in hypnotoad and anim modes.
- [ ] Investigate esp_dsp for rendering acceleration — current rendering is already LUT-optimized, so gains may be limited. Best candidates: (1) restructure spiral/sclera fill to use vectorized batch ops instead of per-pixel branching, (2) speed up one-time LUT init (`spiral_lut_init`, `iris_tex_init`) which do per-pixel `sqrtf`/`atan2f`. The per-frame loops are mostly integer ops + LUT lookups, which don't map cleanly to esp_dsp's DSP primitives.

## 1.75" board (ESP32-S3-Touch-AMOLED-1.75) remaining work
- [ ] ES8311/ES7210 audio codec support — replace PDM mic with I2S codec input so mic_loudness works on the 1.75" board (currently stays 0). Needs I2C codec init + I2S standard mode config with different GPIOs (GPIO9/42/45/8/10).
- [ ] AXP2101 PMIC integration — battery voltage/percentage monitoring via I2C (addr 0x34), charge control, power-off support. Currently battery stats are always 0 on the 1.75".
- [ ] Verify/fix CST9217 touch — minimal driver using I2C addr 0x5A and HYN register protocol. May need address or register adjustments if touch doesn't respond correctly.
- [ ] BLE brightness control — expose display brightness as a BLE param on the 1.75" (writes cmd 0x51 to CO5300). Not applicable to 1.46" which uses GPIO backlight.

## New eye types
- [ ] Lava lamp blob eye — metaball-based barbell shape: two round-ish blobs connected by a bar. Animations: (1) blobs slowly grow/shrink independently, (2) barbell width oscillates (blobs move closer/farther), (3) barbell angle oscillates slowly so it's not always horizontal, (4) smaller satellite blobs periodically try to emerge from the main blob and get slowly reabsorbed. All motions should be slow and organic.
- [ ] Reptile/snake eye — vertical slit pupil that dilates with input, textured golden/amber iris with radial cracks
- [ ] Goat eye — horizontal rectangular pupil
- [ ] Mechanical/cyberpunk eye — concentric iris rings rotating in opposite directions, camera aperture blades that open/close
- [ ] Sauron eye — flaming vertical slit with animated fire/ember particles drifting upward
- [ ] Sharingan/magic rune eye — slowly rotating geometric symbols (tomoe, runes, glyphs) orbiting the pupil
- [ ] Glitch eye — normal eye that periodically corrupts with scan line shifts, color channel offsets, static noise patches
- [ ] Void/space eye — black sclera, tiny bright iris with slow particle/nebula swirl effect
- [ ] Compound/insect eye — hexagonal grid tessellation, each cell offset in color/brightness for a shimmer effect
- [ ] Bloodshot eye — normal eye with animated red veins that slowly crawl/pulse across the sclera
- [ ] Loading spinner eye — pupil replaced with rotating segmented arc, pulsing iris — good as a "thinking" state
