# M213c: Shoreline waves

## Why

FA's beaches have foam rolling in. A map's water block lists **wave
generators**: SCMP_009 alone has 6,939, each with a turbulence texture and
the shoreline ramp. The parser reads them (since fix/scmap-read-whole,
which also made the dry maps and the maps with over 10,000 generators read
whole), but nothing drew them. Moho's
`WaveSystem` makes each generator in view emit a flat, animated,
ramp-coloured particle every few seconds, which drifts shoreward, grows
and fades.

The rules come from faf-re:
- `terrain/water/WaveSystem.cpp`: `WaveGenerator::LoadSerializedState`,
  `RefreshTextureHandlesAndSchedule`, `RebuildSpatialBounds`, `Update`,
  and `WaveSystem::Update`;
- `particles/SWorldParticle.h` and `ParticleRenderBuckets.cpp`: the
  particle and its upload;
- `effects/rendering/CEfxEmitter.cpp`: what the particle's two frame values
  mean;
- `particles/CWorldParticles.cpp`: `AddWorldParticle`.

## The rules

### The record

The water block ends with the four wave-normal textures. After them comes
a count (u32), then per generator:

| Field | Type |
|---|---|
| texture | cstring |
| ramp | cstring |
| position | 3 × f32 |
| angle (radians) | f32 |
| direction (a tick) | 3 × f32 |
| lifetime min, max (ticks) | 2 × f32 |
| interval min, max (seconds) | 2 × f32 |
| begin size, end size | 2 × f32 |
| frame count, frame rate min, frame rate max, strip count (v > 51) | 4 × f32 |

A map of version 51 or older lacks the last four; they default to 1, 1, 0
and 1.

faf-re names the frame count `mRampValueScale` and the strip count
`mTextureSelectionRange`. What they do shows what they are: the particle
takes 1/x of them as `mValue1` and `mValue3`, and `CEfxEmitter` fills those
from 1/`FrameCount` and 1/`StripCount`. The editor's wave presets
(`WaveParameters`) name them `frameCount` and `stripCount`.

### At load (`RefreshTextureHandlesAndSchedule`)

- Its last emission is set to *now − random[0, max interval]*, and its
  interval to random[min, max]. So the generators start out of step.
- Its bounds are the position ± *r* across and ± 0.1 in height, where
  *r* = |direction| × lifetime + max(begin, end) / 2. The lifetime is one
  random draw, taken at load.

### Each frame (`WaveSystem::Update(camera, elapsedSeconds, tick)`)

- It does nothing when there are no generators, or when the frame took
  over 200 seconds.
- On every fifth tick (`tick % 5 == 0`), the generators whose bounds meet
  the camera's view become the ones that emit. faf-re names the argument
  `tick`, and its caller isn't recovered. The engine passes the sim tick,
  as `CWorldParticles::RenderEffects` takes it.
- Each of those runs `WaveGenerator::Update(now)`, on the system clock.

### An emission (`WaveGenerator::Update(now)`)

Once *now − last* passes its interval, the generator emits one particle.
It then draws a new interval, random[min, max], and sets *last* to *now*.

The particle (a `TRampAnimateFlat` particle, blend mode 0 = `ALPHABLEND`)
has:

| Property | Value |
|---|---|
| position | the generator's |
| motion | moves by its direction each tick; no acceleration, no drag |
| lie | flat on the world, turned by the generator's angle |
| size | begin → end over its life |
| lifetime | random[min, max] ticks |
| frame rate | random[min, max] |
| frames | across the texture, each 1/frame count wide |
| strip | floor(random × strip count) / strip count, of 1/strip count height |
| ramp | row 0, by age / lifetime |
| born | when it is added, on the particle clock |

### The world's particles

- A wave is born when `CWorldParticles` uploads it: its `mInterop` starts
  at 0, and the upload adds the particle clock's time.
- `AddWorldParticle` takes no particle once five sim beats have passed
  without a frame. The engine renders every frame it steps, so this never
  applies there.
- Particle textures wrap across (`ParticleSampler0`'s `AddressU = WRAP`).
  So a one-frame wave's frame offset, a whole number, draws the same
  texture. All of SCMP_009's generators have one frame and one strip.

## The engine

- **The parser:** `ScmapWaveGenerator` and `ScmapData::waves` (the
  parser had skipped 68 bytes each). The terrain carries them.
- **`renderer::WaveSystem` (new):**
  - it holds the generators and their schedule;
  - it tests their bounds against the frustum every fifth frame;
  - it emits on the system seconds it is given;
  - its random stream is its own, seeded alike each run, as the particle
    system's is.
- **`ParticleSystem::add_wave`:** a wave joins the particles at the next
  update, born at that frame's render time. Its draw parameters (the frame
  width and strip height, Moho's `mValue1` and `mValue3`) come from a
  blueprint the wave system owns. There is one per texture, ramp, frame
  count and strip count, flat and alpha-blended.
- **`Frustum::is_box_visible`:** Moho collects the generators in view by
  their bounding boxes.
- **The renderer:**
  - It loads the wave system with the scene.
  - Each frame, before the particles, it updates the system with the
    frustum, the frame's step, the sim tick and the waves' clock.
  - That clock is the frame steps summed: the system clock in a game,
    and a test's fixed step in a test.

## Tests

- **Unit:**
  - the frustum's box test;
  - a generator's bounds;
  - the schedule: first within its interval, then each on the first step
    past it;
  - the fifth-tick look, and the 200-second guard;
  - a wave's fields, strips and blueprint;
  - a wave in the particles: born at the next frame, drifting, growing,
    turned, its frame and strip.
- **`--wave-test` (gate), on SCMP_009:**
  - the 6,939 generators reach the renderer;
  - at the shores in the map's middle, those in view emit, and every
    wave drawn lies flat on the water where its generator's drift puts it;
  - the frame differs from the same frame without them;
  - inland, none emit.
- **Mutation:**
  - the record's order and defaults;
  - the schedule, the fifth-frame refresh and the bounds;
  - the particle's fields.
