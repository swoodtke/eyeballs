# TODO

## Open issues
- [x] 1.75" battery — verified 2026-07-07 with a battery wired: AXP2101
  voltage/percent readings are reasonable and the power button turns the
  device on (512 ms press) and off (4 s hold) via PMIC hardware.
- [ ] BLE writes hit globals with no sync against the render loop — mostly
  cosmetic, but a BLE mode write can discard a simultaneous touch mode change,
  and mode is the only writable param now. Cheap fix: apply BLE writes on the
  main loop side.
- [ ] Migrate from the legacy `driver/i2c.h` API to `i2c_master` — the
  deprecation CRITICAL warning appears in the CMake configure stage (not
  in incremental builds). Touches the shared i2c helpers used by TCA9554,
  QMI8658, ES7210, CST9217, and AXP2101.

## Eye-to-eye sync (goggle configuration)
Implemented over BLE dual-role (~1-3 mA vs ESP-NOW's always-listening WiFi at
~60-90 mA). The left eye (role 0) is a central: it scans for a right eye
advertising the same group id in manufacturer data, connects, bonds, and
writes 8-byte packets to the Sync Data characteristic (0x0040); the right eye
notifies its own events back on the same characteristic. BLE params
0x0032/0x0033 set group + role (persisted; group 0 = off). Either eye can
announce a mode change (last-writer-wins), the left beacons mode + animation
clock at 5 Hz (peer snaps when drift > 50 ms), and blinks apply with a
20–50 ms receiver jitter.
- [ ] Later: mic beat-sync events ride the same channel
- [ ] Later: hypnotoad spiral phase sync (only the pre-rendered animation
  clock syncs today; hypnotoad advances per-frame)

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
