# TODO

## Code review findings (2026-07-02)

### Must fix
- [x] BLE Display Mode write crash — fixed: `ble_display_mode` is clamped to `< NUM_MODES` before the cast (falls back to Cat Eye on an out-of-range write).
- [x] BLE Brightness param silently missing — fixed: `MAX_PARAMS` raised to 40 and `ble_init()` logs an error instead of silently truncating.
- [x] Stack overflow on malformed animation data — fixed: `anim_init()` rejects `pal_size` outside 1–64 before reading the palette.

### Should fix
- [x] 1.75" last flush strip exceeds `max_transfer_sz` — obsolete: strip-based flushing was replaced by a single draw_bitmap chunked by the SPI layer.
- [x] `mic_sensitivity` ≤ 1.0 breaks audio reactivity — fixed: the divisor uses the sensitivity clamped to ≥ 1.05.
- [x] PSRAM leak in `spiral_lut_init` — fixed: partial allocations are freed via `spiral_lut_free()` on failure.
- [x] Last animation frame decoded with wrong compressed size — fixed by the decode-on-demand loader: the last frame is bounded by `anim_size` from the partition TOC.
- [x] Device rename silently fails on multi-byte characters — fixed: `saveName()` trims whole characters until the name fits in 20 UTF-8 bytes.
- [x] `logBattery()` matches characteristics by label string — fixed: matches on UUIDs 0022/0023/0024.

### Repo hygiene
- [x] `git rm -r mnt/` — removed
- [x] `git rm EyeballController/build.log` and gitignore it — removed and ignored

### Minor
- [ ] No BLE pairing/bonding — anyone in range can rename/control the device (and trigger the mode-write crash). Consider `BLE_GATT_CHR_F_WRITE_ENC` + bonding, or accept as a conscious choice.
- [ ] `EyeballDevice.swift:29` uses `load(as: Float.self)` on `Data` — can trap on unaligned memory; use `loadUnaligned(as:)`.
- [ ] BLE writes hit globals with no sync against the render loop — mostly cosmetic (one garbled color frame), but a BLE mode write can discard a simultaneous touch mode change.
- [ ] Dead code: `CharacteristicEntry.dataLength` frozen at init and unused. (`draw_cat_pupil` has been removed; `te_sem` is now consumed by the TE-synced flush.)
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
