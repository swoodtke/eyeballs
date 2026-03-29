# TODO

- [x] Use both ESP32-S3 cores — rendering on Core 1 with double-buffered framebuffers, sensors/physics on Core 0. ~29→39 FPS.
- [ ] Investigate esp_dsp for rendering acceleration — current rendering is already LUT-optimized, so gains may be limited. Best candidates: (1) restructure spiral/sclera fill to use vectorized batch ops instead of per-pixel branching, (2) speed up one-time LUT init (`spiral_lut_init`, `iris_tex_init`) which do per-pixel `sqrtf`/`atan2f`. The per-frame loops are mostly integer ops + LUT lookups, which don't map cleanly to esp_dsp's DSP primitives.

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
