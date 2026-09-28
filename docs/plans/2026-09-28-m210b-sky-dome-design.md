# M210b: The sky dome

## Why

The engine clears the frame to a flat blue-grey where the sky should be.
FA draws the map's own sky: a dome over the map, a gradient from the
horizon to the sky's colour, four drifting cirrus layers, and sky decals
(the sun, planets, bright clouds), which glow into the bloom.

The sky shows wherever the world doesn't cover the frame:

- over the map's edges;
- above the horizon, whenever the camera tilts toward it.

Every retail map (all 60 are format 60) carries its own sky block, which
the parser skipped.

The rules come from:

- faf-re's `SkyDome` (`Load`, `SetupHorizonAndCirrus`,
  `CreateDomeVertexBuffer`/`IndexBuffer`, `RenderAtmosphere`,
  `RenderCirrus`, `RenderDecals`, `RenderCumulus`);
- `WRenViewport::RenderSkyDome` and `WRenViewport::Render`, for the passes'
  order and the clear;
- `CWldTerrainRes::Load`, for where the block sits in a `.scmap`;
- FA's `sky.fx`, for the shaders.

Retail's files are read for the rules, never copied.

## The rules

### The `.scmap` sky block

The block comes from map version 58 (0x3A) on, after the terrain types.
It is:

- the dome's origin (3 floats);
- its shape: elevation (the horizon's start), radius, and start angle
  (3 floats);
- its width and height in segments (2 ints);
- the horizon's size (a float);
- the horizon's colour and the sky's colour (3 floats each);
- the decals' glow multiplier (a float; faf-re's `mHorizonBlend`);
- the decals' albedo texture and glow texture (strings);
- the decals: a count, then 40 bytes for each. Each decal is:
  - its position (xyz), and its rotation;
  - its size (w, h);
  - its UV rectangle (u, v, width, height) in the atlas;
- three cumulus texture paths (strings, empty on retail maps);
- the cirrus multiplier and the cirrus colour (4 floats);
- the cirrus texture (a string);
- a layer count, which Moho ignores, then 4 layers of 20 bytes. Each layer
  is:
  - its frequency (2 floats);
  - its speed;
  - its direction (2 floats).

From version 59 on, the cartographic decal batches follow the sky block.
There is a count, then each batch:

- a technique name, from version 60;
- a texture path;
- a count, then 36 bytes for each decal.

The props come after the batches.

A map older than version 58 gets the sky that `SetupHorizonAndCirrus`
derives from the map (these are the `CWldTerrainRes::Reset` path's
numbers):

- **Origin:** the map's centre, at y 0.
- **Radius:** the half-diagonal ÷ cos(72°).
- **Elevation:** the water's, or the terrain's floor on a map without water.
- **Start angle:** 1.2566371, with 16 × 6 segments.
- **Horizon:** size radius × 0.1536, colour (0.81, 0.74, 0.64).
- **Sky:** (0.26, 0.46, 0.59).
- **Cirrus:** multiplier 1.8, colour (1.39, 0.76, 0.49), texture
  `cirrus001_512.dds`, and the static four-layer table.
- **No decals.**

(faf-re's older-map branch reads the half-extent from Y, not Z. That looks
like a recovery slip; no retail map takes it.)

### The dome (`CreateDomeVertexBuffer`/`IndexBuffer`)

- `height` rings of `width + 1` vertices, then an apex. Ring `r` sits at
  angle `a = start + r × (π/2 − start) / height`.
  - Its radius is `R cos a`, where `R = radius / cos start`.
  - Its height is `R sin a − R sin start`, plus the origin's y and the
    elevation.
  - Its vertex `c` sits at azimuth `θ = 2π c / width`.
- A vertex carries its position and θ.
- The indices are:
  - each ring pair: `(b+c+1, b+c+width+1, b+c)` and
    `(b+c+width+1, b+c+1, b+c+width+2)`;
  - then a fan to the apex.

  They are 16-bit, as Moho's are.
- A dome with more vertices than 16-bit indices reach isn't drawn.

### The passes (`RenderSkyDome`)

For each world view, after the per-head clear to black (colour 0), and
before the terrain:

1. **Atmosphere**, over the dome.
   - It maps θ/2π to the lookup's row at v 0.25, and the elevation's place
     between the horizon's start and end to the row at v 0.75. The horizon
     ends at `elevation + horizon size`.
   - The two lookups' alpha multiply to `t`, and the colour is
     `lerp(horizonColor, skyColor, 1 − t)`.
   - The lookup is retail's `horizonLookup.dds`, an A8 texture sampled
     point and clamp. Its θ row is all 255; its elevation row falls from
     255 to 1.
   - It writes RGB, with no blend.
2. **Decals**, when the map has decals and both textures. Each decal is a
   billboard:
   - its corners `(±1, ±1)`, scaled by its size and rotated by its angle;
   - laid along the view's right and up axes at its position;
   - UV from its rectangle, with v flipped.

   The two passes:
   - albedo, blended SrcAlpha/InvSrcAlpha into RGB;
   - glow, `multiplier × glow.a`, written into alpha alone. That alpha is
     the frame's glow, which the bloom reads.
3. **Cirrus**, over the dome again.
   - Each layer's coordinate is `frequency × (R(dir)·xz − time × speed ×
     dir)`, with `time = tick + interpolant` and `dir` normalised.
   - The four layers sample the cirrus texture's R, G, B and A. Their
     product times the multiplier is the alpha of `cirrusColor`, blended
     SrcAlpha/InvSrcAlpha into RGB.
4. **Cumulus**: the dispatcher hands it an empty cloud list, so it never
   draws. The engine leaves it out.

Every pass:

- has no depth test and no depth write;
- culls clockwise triangles (D3D's `CullMode = CW`). From inside the dome
  the sky is drawn; from above it, it isn't, and the black clear shows
  past the map's edges.

## The engine

- `map::ScmapSky` (`scmap_parser.hpp`) holds the block.
  - `parse_scmap` reads it, and the cartographic batches, instead of
    skipping a fixed number of bytes. An older map gets
    `map::default_sky(...)`.
  - `Terrain` carries it (`set_sky` / `sky()`).
- `renderer::build_sky_dome(const ScmapSky&)` (`sky_dome.{hpp,cpp}`) is
  pure: Moho's vertices and indices, for tests.
- `renderer::SkyRenderer` (`sky_renderer.{hpp,cpp}`), shaped like
  `WaterRenderer`:
  - `init` makes the pipelines (atmosphere, cirrus, decal albedo, decal
    glow), the samplers and each frame's uniforms;
  - `build(terrain, textures)` makes the dome, the decals and the
    textures;
  - `update(camera, vp, tick, interpolant, fi)` sets the frame;
  - `record(cmd, fi)` draws, first in the scene pass.
- `SkyUniforms` (std140) holds:
  - viewProj, the view's right and up;
  - the horizon's start and end, and its colour and the sky's;
  - time, the cirrus multiplier and colour, and the four layers;
  - the decals' glow multiplier.
- Every sky vertex shader sets `gl_Position.z = 0.5 w`. Moho's dome lies
  inside its far plane (it is never clipped). The engine's far plane
  (5000) could clip a big map's dome (its radius on a 4096-unit map is
  9372), and with no depth test, a fixed depth changes nothing else.
- Every sky pass saturates what it writes. Moho's target is 8-bit, so a
  map colour past 1 (SCMP_005's sky blue is 1.19) and the cirrus alpha
  (up to 1.8 × its product) are clamped before blending. The engine's scene
  is half floats, and would keep them.
- The DDS parser takes A8 (`DDPF_ALPHA`, 8 bits). It becomes an R8 image
  whose view swizzles to (0, 0, 0, R), as D3D samples A8.
- The scene clears to black, alpha 0, as Moho's head does
  (`Renderer::kClearColor`).
  - The render tests' `OffscreenShots` sets the old blue-grey as its
    backdrop (`set_clear_color`). Their plates stand off their ground, and
    the tests measure them against it.
  - `--sky-test` clears as a game does.

## Tests

- **`test_sky_dome` (unit):**
  - the vertex and index counts;
  - each ring's vertices against the formula;
  - the apex;
  - the first quad, the next ring pair and the fan;
  - every vertex used;
  - the shapes it refuses;
  - `default_sky`'s numbers.
- **`test_map` (unit):** a version-56 map gets `default_sky`, from its
  water or its terrain's floor.
- **`test_dds_parser` (unit):** A8 reads as alpha alone.
- **`--sky-test` (gate)**, on SCMP_009:
  - Its block reads to the values dumped from the file, and all 5182 props
    are still found after it and the cartographic batches.
  - **Atmosphere** (cirrus and decals off): looking north over the map's
    edge, each sky pixel of the middle column matches `AtmospherePS` where
    the pixel's ray meets the dome's own triangles, with retail's lookup
    point-sampled. 64 of 64 match, the worst error 0.0005, over t from 1
    (the horizon's colour) to 0.01 (the sky's). No glow is written.
  - **Past the far plane:** a dome four times as wide (radius 9373) matches
    the same way, 58 of 58.
  - **Cull:** from above the dome, past the map's corner, every pixel over
    the dome is black (943 of 943).
  - **Decals** (cirrus off): aimed at a decal low in the sky whose glow has
    alpha.
    - At six points across its billboard, each pixel is the atlas's texel
      at DecalVS's UV over the atmosphere, and its alpha is the multiplier
      times the glow's. The pixel's ray is met on the billboard's plane,
      turned back by the rotation, and the atlas sampled bilinearly. All 6
      match, the worst off by 0.0005.
    - The glow lands in alpha, at most the multiplier.
    - The glow pass leaves the colour alone.
  - **Cirrus:** it changes the bare sky, drifts in 300 ticks, and writes no
    glow.
  - **Cirrus saturated:** at frequency 0 every layer reads the texture's
    corner, so with a multiplier of 1000 the alpha is far past 1, and every
    sky pixel is the colour (2, 0.5, 0.25) saturated.
- **Mutation:** all 23 mutants are killed:
  - the cull removed, and the cull flipped;
  - the lookup's rows;
  - `t` for `1 − t`;
  - the glow pass writing colour, the cirrus writing alpha, the albedo
    unblended;
  - the old clear colour;
  - the ring formula without its base;
  - the horizon's end as its size;
  - the cirrus without time, and unsaturated;
  - the dome's far-plane pin;
  - the colours read in the wrong order;
  - the decals' v unflipped, rotation sign, axes swapped, not drawn, and
    glow multiplier 1;
  - the sky not drawn;
  - A8 not read as alpha;
  - the cartographic batches unread;
  - an older map's floor as 0.
