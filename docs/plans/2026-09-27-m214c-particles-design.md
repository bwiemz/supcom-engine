# M214c: FA's particles

## Why

The particle system only approximated FA's emitters:

- It sampled curves over a 0..1 fraction of the emitter's lifetime.
- It emitted and moved particles frame by frame on the CPU.
- It faded the last fifth of every life, which FA never does.
- It had two blends where FA has five, and ignored bones, local frames,
  drag and the water.
- It never found an animated texture: it read `TextureFrameCount`, but
  retail's field is `TextureFramecount`.

Particles are in every muzzle flash, explosion and plume.

FA's `CEfxEmitter` emits once a sim tick into the world's particle buffer.
particle.fx's `WorldVS` then moves each particle analytically from its spawn
state. The rules come from these faf-re functions, plus particle.fx's
`WorldVS`, `WorldVSAlign`, `WorldPS` and `TRamp*` techniques:

- `CEfxEmitter`: `Tick`, `OnTick`, `UpdateCurve`, `CanSeeCam`, `IsVisible`,
  `ProcessLifetime`, `Interpolate`;
- `SEfxCurve::GetValue`;
- the `REmitterBlueprint` reflection;
- `UploadPendingParticlesIntoWorkItem`.

Retail's files are read for the rules, never copied.

## The rules

### Blueprint

`EmitterBlueprint { … }` uses these reflected names and defaults:

- Numbers: `Lifetime` 0, `Repeattime` 0, `TextureFramecount` 0,
  `TextureStripcount` 1, `Blendmode` 0, `LODCutoff` 100, `SortOrder` 0.
- Flags on by default: `LocalVelocity`, `EmitIfVisible`, `CatchupEmit`,
  `InterpolateEmission`, `SnapToWaterline`.
- Flags off by default: `LocalAcceleration`, `Gravity`, `AlignRotation`,
  `AlignToBone`, `Flat`, `CreateIfVisible`, `ParticleResistance`,
  `OnlyEmitOnWater`.
- Textures: `Texture`, `RampTexture`.
- 21 curves of `{ x, y, z }` keys, kept in order of x.

### Curves

Keys are in ticks.

- `GetValue(x)` takes the first key past x.
- Before the first key, or past the last, it uses that key. Between two keys
  it interpolates y and z.
- The result is y + (random − 0.5)·z, so z is the full width of the spread.
- A `Repeattime` of 0 reads each curve's first key (a NaN phase, as in Moho).

### Clock

Each emitter has a clock (TICKCOUNT) that starts at 0 and advances one tick
each tick it emits. It reads its curves at `fmod(clock − ticks back + cursor,
Repeattime)`.

### Emission

Each tick, `EmitRate` accumulates and its whole part is emitted. With
`InterpolateEmission`, the particles are spread over the tick at cursor k/n.
Each particle gets:

- **Position:** `POSITION` + scale·(`XPos`, `YPos`, `ZPos`), placed by the
  emitter's frame at the cursor (its bone, between last tick's pose and this
  one's). Then:
  - `SnapToWaterline`: y at least the water's (or at most, for `SortOrder` < 0).
  - `OnlyEmitOnWater`: nothing is emitted over land; on water, y is the
    water's.
  - Scatter: moved across the ground by (random − 0.5)·scale·`Size`, in a
    random direction.
- **Acceleration:** scale·`X/Y/ZAccel`, in the frame's axes when
  `LocalAcceleration`; −0.02 on y with `Gravity`.
- **Velocity:** scale·`X/Y/ZDirection`, in the frame's axes when
  `LocalVelocity`, times `Velocity`.
- **The rest:**
  - `Resistance`;
  - `Lifetime` (at least 0);
  - scale·`StartSize` and scale·`EndSize`;
  - `RampSelection`;
  - `FrameRate`;
  - floor(`TextureSelection`)/strips;
  - `InitialRotation`° and `RotationRate`° a tick.
  - With `AlignToBone`: a flat particle is turned to the bone's +Z, and any
    other has the bone's +Z as its velocity.

A particle is born at time `tick − ticks back + cursor` on the render clock.
An emitter's frame for tick k shows at time k + 1, so particles leave the
entity where it is drawn.

### Motion

With t = time − born, in ticks:

- **Position:** P + V·t + ½A·t². With drag of resistance r (when
  `ParticleResistance` is set), (A/r² − V/r)(e^(−rt) − 1) + A·t/r + P.
- **Size:** begin + (end − begin)·t/life.
- **Angle:** angle + spin·t.

A particle is drawn while t < its life, even at negative t: a particle born
later in the tick than the frame is drawn back along its path, as Moho does.

### Quads

The corners are at ±size along two axes:

- **Billboard:** the camera's right and up, turned by the angle.
- **Flat:** the world's X and Z, turned by the angle.
- **Align** (`AlignRotation`, `WorldVSAlign`): along its motion
  normalize(V + A·t), and across by cross(view axis, motion).
- **AlignToBone** (not `Flat`): as Align, but it doesn't move.

### Shading

Shading is `WorldPS`: texture × ramp.

- **Texture:** U wraps, V clamps. An animated texture shows frame
  floor(rate·t) of `TextureFramecount` across, in its strip down.
- **Ramp:** clamped, read at (t/life, `RampSelection`).

### Blends

These are the `TRamp` techniques, the same five as `TPolyTrail`'s. All write
RGB only; ADD is SrcAlpha/One. REFRACT (5) is M214d.

### Seen

With `EmitIfVisible`, an emitter emits only while:

- its position is within `LODCutoff` of the camera;
- a 5-unit sphere around it is in view;
- the focus army has LOS there, checked every fifth tick.

Its position is set when it's made, then every third tick. Missed ticks are
caught up, min(missed, the longest particle life, 24), from its frame
history; its clock stands while it misses them. With `CreateIfVisible`, an
emitter the player can't see on its first tick is never made (M215b), and
the overlay never marks it.

Ticks the sim runs between two frames (a slow frame shows several at once)
were Moho's to emit, so the renderer catches them up too. Unlike missed
ticks, they move the emitter's clock on.

### Order and water

Beams draw first, then particles, then trails (`RenderEffects`). Particles
are bucketed by `SortOrder`, then textures, then blend. Those with
`SortOrder` < 0 draw before the water.

### Overlay

The overlay drops its dot for every emitter the particle system draws.

### Lifetime

An emitter's end stays the sim's (M224d): its `Lifetime` from creation. The
renderer emits while the effect lives.

## The engine

- **Sim.**
  - At-emitters record their bone's world transform when made
    (`IEffect::set_frame`; `Unit::bone_world_rotation`).
  - `EffectRecord` carries each emitter's frame for the tick: an attached
    emitter's bone as posed, an At-emitter's creation frame, or the world's
    for one without an entity.
- **Blueprints.** `EmitterBlueprintCache` loads through the shared runner,
  with Moho's defaults and curve rules.
- **`ParticleSystem`.**
  - For each new snapshot tick, it keeps each emitter's frame history,
    clock, owed fraction and misses, and emits `SWorldParticle`-like records
    into the world's particle list.
  - Each frame, it drops dead particles and places the rest as quads (centre,
    two axes, UV rectangle, ramp coordinate), in draw order and in runs.
- **`ParticleRenderer`.** Instanced quads with one pipeline per blend,
  drawn in two passes (under the water, and after the beams).

## Tests

Unit tests (`[emitter]`):

- `GetValue`'s reads and spread;
- the emit rate's carried fraction (2.5: 2, 3, 2, 3…);
- a pulsed rate read at the clock modulo `Repeattime`;
- analytic motion;
- `LocalVelocity` through a turned frame.

`--particle-render-test` (gate), with the test's own blueprints:

1. Emission per tick, and particles' ages and lives; an ended emitter's
   particles live on.
2. Motion with acceleration and gravity; drag.
3. `LocalVelocity` and `LocalAcceleration` through the unit's frame, and
   through a bone turned by a rotator.
4. An attached emitter follows its bone; an At-bone emitter stays where it
   was made.
5. Interpolated emission spreads a moving emitter's particles along its path
   and over its tick.
6. Quads: billboard, flat, aligned to their motion, and along the bone
   without moving (AlignToBone); sizes over life.
7. An animated texture's frames and strips; the ramp by age and selection.
8. `SnapToWaterline` and `OnlyEmitOnWater`.
9. `EmitIfVisible`: LOD, view and fog, and the catch-up. A
   `CreateIfVisible` emitter made in the fog stays unmade, and unmarked.
10. Pixels: ADD reddens, MODULATEINVERSE darkens; the under-water pass.
11. No overlay dot for a drawn emitter (an unreadable one keeps its dot);
    retail's `aeon_build_01` draws.
12. `Size` scatters particles across the ground, up to half of it.
13. A slow frame showing three ticks at once emits all three, and the
    emitter's clock moves on with them.

The effect-intel test (M215b) now looks from within its emitters' LODCutoff.

## Left for later

- REFRACT, blend mode 5 (21 retail blueprints), which reads the frame
  behind it.
- `SetEmitterCurveParam` at run time (two retail calls).
- The fidelity flags (`LowFidelity`, `MedFidelity`, `HighFidelity`).
- Moho measures LODCutoff along the camera's view (a fade plane). The
  engine measures distance from the eye, as for beams and trails.
