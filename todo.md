# TODO

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
