# TODO

## Open issues
- [ ] 1.75" battery — no battery is physically attached yet. The AXP2101
  driver has been fixed (correct VBAT registers, E-gauge percentage, button
  power-on 512 ms / power-off 4 s hold in PMIC hardware), but it's untested
  against a real battery: once one is wired, verify voltage/percent/charging
  in the PWR log and that the button powers the board on from battery and
  off with a 4 s hold. The init log's VBUS millivolt reading (~5000 on USB)
  proves the ADC path works meanwhile.
- [ ] BLE writes hit globals with no sync against the render loop — mostly
  cosmetic, but a BLE mode write can discard a simultaneous touch mode change,
  and mode is the only writable param now. Cheap fix: apply BLE writes on the
  main loop side.
- [ ] Migrate from the legacy `driver/i2c.h` API to `i2c_master` — the
  deprecation CRITICAL warning appears in the CMake configure stage (not
  in incremental builds). Touches the shared i2c helpers used by TCA9554,
  QMI8658, ES7210, CST9217, and AXP2101.

## Eye-to-eye sync (goggle configuration)
Two eyeballs must coordinate when worn as a pair. Chosen transport: **ESP-NOW**
(WiFi peer-to-peer) — ~1–2 ms latency, works with no phone present, coexists
with BLE, and consumes no BLE connection slots.
- [ ] Pairing: a BLE param (persisted to NVS) assigns a group id + role
  (left/right); leader = left eye
- [ ] Leader broadcasts mode changes, the animation clock (anim start offset),
  and blink/expression triggers; follower applies them
- [ ] Blink timing: follower offsets blinks 20–50 ms for an organic feel
- [ ] Later: mic beat-sync events ride the same channel

## Features
- [ ] Re-add BLE params as they prove useful — per-mode options land in the
  app's Mode Options section automatically; add `labelForUUID` and
  `sliderRange` cases per param
- [ ] Music beat sync for the heart (see eyeball_ideas.md) — bass-band onset
  detection restarting the heart loop, adaptive threshold for loud venues
- [ ] GATT User Description descriptors so new firmware params display in the
  app without app changes
- [ ] Speaker support on the 1.75" (ES8311 DAC — its init code was removed
  2026-07-07 as dead code; recover from git history when needed)

## Performance (only if a mode needs it)
- [ ] Hypnotoad still does per-pixel work on the ~21% of pixels outside the
  display circle — use per-row x-bounds like the animation blit does
- [ ] Spiral mode sits at ~20 FPS due to TE quantization; reaching 29.6 needs
  render+write under 33.8 ms (candidates: faster RLE bit reader, writing both
  sibling rows in one pass). Playback speed is already correct via wall-clock
  frame selection.

## App polish
- [ ] `subscribeToCharacteristics()` rebuilds all Combine subscriptions on
  every characteristic append — harmless at 4 characteristics, revisit if the
  param count grows again
- [ ] Package tests (`Tests/EyeballBLETests`) — DeviceRegistry and
  CharacteristicEntry decoding are natural first targets

## New eye types (backlog)
- [ ] Lava lamp blob eye — metaball-based barbell shape: two round-ish blobs connected by a bar. Animations: (1) blobs slowly grow/shrink independently, (2) barbell width oscillates (blobs move closer/farther), (3) barbell angle oscillates slowly so it's not always horizontal, (4) smaller satellite blobs periodically try to emerge from the main blob and get slowly reabsorbed. All motions should be slow and organic.
- [ ] Reptile/snake eye — vertical slit pupil that dilates with input, textured golden/amber iris with radial cracks
- [ ] Goat eye — horizontal rectangular pupil
- [ ] Mechanical/cyberpunk eye — concentric iris rings rotating in opposite directions, camera aperture blades that open/close
- [ ] Sharingan/magic rune eye — slowly rotating geometric symbols (tomoe, runes, glyphs) orbiting the pupil
- [ ] Glitch eye — normal eye that periodically corrupts with scan line shifts, color channel offsets, static noise patches
- [ ] Void/space eye — black sclera, tiny bright iris with slow particle/nebula swirl effect
- [ ] Compound/insect eye — hexagonal grid tessellation, each cell offset in color/brightness for a shimmer effect
- [ ] Bloodshot eye — normal eye with animated red veins that slowly crawl/pulse across the sclera
- [ ] Loading spinner eye — pupil replaced with rotating segmented arc, pulsing iris — good as a "thinking" state
