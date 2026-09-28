# M212f: TTerrainGlow, the lava map's terrain

## Why

One retail map, Varga Pass (SCMP_023), draws its terrain with
terrain.fx's `TTerrainGlow` technique rather than `TTerrain` or
`TTerrainXP`. It is FA's lava: its second stratum scrolls slowly, and that
stratum's alpha is written as the frame's glow, which bloom spreads.

The engine draws every map that isn't `TTerrainXP` as `TTerrain`, so the
lava stands still and doesn't glow. It couldn't have mattered before
fix/scmap-read-whole: SCMP_023 is a dry map, and its strata weren't read at
all.

The rules come from:
- terrain.fx's `TerrainGlowVS`, `TerrainGlowPS` and the `TTerrainGlow`
  technique, read for their formulas;
- faf-re's `HighFidelityTerrain::UpdateRenderContext`, for when the
  shader's `Time` changes;
- `VTransform::Compare`.

## The rules

### The technique

`TTerrainGlow` is `TTerrain` (four strata over the lower albedo, the upper
stratum over them, `CalculateLighting`) with three changes:

1. **Stratum 1 scrolls.** Its texture coordinates are offset by
   `0.01 × (sin(Time/8), cos(Time/8))` (`TerrainGlowVS`:
   `sincos(Time * 0.125, offset.x, offset.y); offset *= 0.01`).
2. **Its alpha is the glow, not the specular.**
   - `glow` is stratum 1's alpha sampled at the swapped offset
     `(cos, sin)`.
   - Stratum 1's own alpha is zeroed before the blend, so where it lies
     the specular amount (`1 − albedo.w`) is full.
3. **The frame's alpha is the glow:** `glow × mask.y + 0.01`, where
   `mask.y` is stratum 1's sharpened mask. It replaces `TTerrain`'s
   specular glow.

Its normals are `TTerrainNormals`, as `TTerrain`'s are.

### The shader's Time

- `HighFidelityTerrain::UpdateRenderContext` sets the terrain's `Time` to
  *game tick + the frame's interpolant*.
- It does so only when it re-tessellates: when the camera's transform
  differs from the last one bit for bit (`VTransform::Compare` is
  `pos != pos || orient != orient`), or the decal manager has pending
  changes.
- So in FA the lava stands still while the camera does, and moves on
  when the camera moves or a decal comes or goes. The port keeps that.

## The engine

- **The light block:** `sunAmbience.w` names the technique: 0 `TTerrain`,
  1 `TTerrainXP`, 2 `TTerrainGlow`.
  - `terrainXP()` is 1 only.
  - `terrainGlow()` is 2.
- **The terrain's push block:** its spare float carries `Time`.
- **The renderer:**
  - It keeps the terrain's time and the camera transform it was last set
    at.
  - Each frame, if the camera's eye or orientation changed bit for bit,
    or the runtime decals changed, it sets the time to *tick + alpha*.
- **The terrain shader:**
  - With `terrainGlow()`, it offsets stratum 1's coordinates, zeroes its
    alpha, and takes the glow.
  - It writes `glow × m0.g + 0.01` as the alpha.

## Tests

- **`--terrain-glow-test` (gate), on SCMP_023, offscreen:**
  - The map reads as `TTerrainGlow`, and the light block says 2.
  - Where stratum 1 lies, the frame's alpha is its alpha plus 0.01. The
    test compares a frame's glow with the stratum's texture sampled at the
    offset.
  - With the camera still, frames several ticks apart are the same.
    Moved, the lava scrolls: the frame differs where stratum 1 lies, and
    not elsewhere.
  - A `TTerrain` map (SCMP_009) writes no such glow.
- **Unit:** the time update's triggers (a moved camera, a changed decal,
  neither).
- **Mutation:** the offset's scale, rate and sin/cos order; the swapped
  glow lookup; the zeroed alpha; the glow's mask; the base 0.01; the
  triggers.
