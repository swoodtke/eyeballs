# Eyeball Display Ideas

Ideas for new display modes across the two configurations: paired displays in a
goggle setup, or a single display running stand-alone (cyclops or chest pendant).

Existing modes: Cat Eye, Hypnotoad, Blob Eye, Sauron.

## More eyes (work in both configs, shine in goggles)

- **Human eye** — white sclera with veins, iris with radial texture (reuse the
  iris LUT), round pupil that dilates with ambient conditions or a BLE param.
  Add saccades: small random jumps between fixation points rather than smooth
  drift — it's what makes eyes read as "alive."
- **Reptile/dragon eye** — vertical slit like the cat but with a fiery or golden
  iris and a nictitating membrane that sweeps horizontally on blink instead of
  vertically. Reuse slit-pupil drawing from cat/Sauron.
- **Goat/octopus eye** — horizontal rectangular pupil. Deeply unsettling, and
  cheap: it's the cat pupil rotated 90°.
- **Compound insect eye** — hex-tessellated ommatidia with a specular highlight
  that moves with the IMU. A static hex grid with a shifting bright spot is
  cheap to render.
- **Robot/HAL eye** — concentric rings, glowing red core, scan lines, occasional
  "iris aperture" open/close like a camera shutter. Animating aperture blades
  is a strong mechanical look.
- **Glitch eye** — any of the above, occasionally corrupted: horizontal tear
  lines, palette inversion for a few frames, static bursts. Cyberpunk costume
  material.
- **Portal/void eye** — pupil is a starfield or swirling nebula instead of
  black. Pairs naturally with the hypnotoad spiral math.

## Two-display-specific ideas (where pairing matters)

The strongest 2-display effects come from the eyes *not* being identical:

- **Convergence/vergence** — both eyes track the same 3D point, so pupils angle
  inward when "looking" at something close. Even faked with a BLE-synced
  focus-distance param, this makes the pair read as one creature instead of two
  screens.
- **Coordinated blinks with jitter** — blink together but offset by 20–50 ms
  randomly. Perfectly simultaneous blinks look mechanical.
- **Winking and asymmetric expressions** — one eye squints while the other
  widens (suspicion), both narrow (anger), both widen (surprise). A small
  "expression" enum synced over BLE.
- **Heterochromia mode** — different iris color per eye, one BLE tap.
- **Ping-pong animations** — something travels from one display to the other:
  a comet that exits stage right on the left eye and enters stage left on the
  right eye, a Cylon/KITT scanner sweeping across both, or Pong actually
  played across the two screens.
- **Dice** — goggles become a giant pair of dice; shake your head (IMU) to
  roll them.

Cross-display effects need a sync channel. Since both boards are BLE
peripherals, simplest is the Mac/iOS app acting as conductor; ESP-NOW between
the two boards would give low-latency direct sync without a central.

## Single-display ideas (cyclops / pendant)

Cyclops is still an eye, so everything above applies — the pendant use case
opens non-eye territory:

- **Heartbeat** — anatomical or stylized heart beating at a BLE-settable BPM.
  On the 1.75" board the mic could make it pulse with music. Maybe the single
  best pendant mode.
- **Audio visualizer** — the I2S mic capture with attack/decay smoothing
  already exists. Radial spectrum bars, a circular waveform ring, or the
  hypnotoad spiral speed driven by volume.
- **Arc reactor** — Iron Man chest piece. Concentric glowing rings, slow
  pulse, flicker on tap. The round display is literally the right shape.
- **Portal hole** — the pendant looks like a hole through your chest: swirling
  void, or a slowly rotating tunnel (spiral LUTs again).
- **Analog clock / astrolabe** — round display, obvious fit. A "wizard clock"
  with moon phases and drifting constellations fits a costume better than a
  plain clock face.
- **Lava lamp / plasma blob** — metaballs or classic plasma palette-cycling;
  the blob-eye asset pipeline could feed this with pre-rendered frames.
- **Mood ring** — slowly shifting color field, with touch setting the "mood,"
  or IMU activity influencing it (frantic movement → red, stillness → blue).
- **Fireplace / candle** — a cropped, calmer variant of the Sauron fire
  animation makes a nice ambient pendant mode.
- **Radar sweep / sonar ping** — rotating sweep line with fading blips. Blips
  could spawn from mic transients so it reacts to nearby sound.
- **Water/gyro toys** — IMU-driven: a bubble level, liquid that sloshes when
  you move, sand that pours, or a marble rolling in a dish. Delightful because
  they respond physically to the wearer.

## Music beat sync (festival use)

The heart animation is one lub-dub cycle looped, so syncing to music needs no
new assets — only firmware playback control:

- **Onset-triggered**: detect each bass hit (50–150 Hz Goertzel/FFT energy vs
  a rolling-average threshold) and restart the loop from frame 0, with a
  refractory period. Heart slams on every kick drum.
- **Tempo-matched**: estimate BPM and scale the frame-advance rate so one loop
  spans one beat period. Smoother; keeps beating through quiet passages.

Loud venues are fine: MEMS mics overload around 120 dB SPL vs ~95–110 dB at a
festival. If it clips near the stacks, lower the ES7210 input gain (could be a
BLE param) — and the adaptive threshold self-calibrates to venue volume. The
same trigger could drive other modes (hypnotoad speed, ring pulse in the
spiral).

## Sensor-driven behaviors that upgrade everything

Layers onto existing modes rather than new modes:

- **Mic-reactive pupils** — pupil dilates on loud sounds, eye "startles"
  (snaps wide, looks toward nothing) on sharp transients like a clap.
- **Touch interactions** — poke the eye and it flinches, squeezes shut, then
  warily reopens. Tap currently cycles modes; a long-press or double-tap could
  trigger reactions instead.
- **Idle behaviors** — after N seconds of no IMU motion, the eye gets sleepy:
  lids droop, slow blinks, eventually closes and "sleeps" until moved. Huge
  personality payoff for little code.
- **Attract/demo mode** — cycles through all modes every 30 s, nice for
  showing off the hardware.

## Suggested priorities

1. **Saccades + idle/sleepy behavior** — biggest realism win for existing eyes.
2. **Heartbeat + audio visualizer** — best pendant modes; mic pipeline already
   exists.
3. **Synced blinks/expressions over BLE** — makes the goggle pair read as one
   creature.
