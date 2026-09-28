# M212e: Moho's terrain normals

## Why

The engine lights the terrain by a normal it builds in each pixel:

- the normal of the terrain mesh's vertices (central differences of the
  heights), turned by Gram-Schmidt into a basis;
- the strata's normal maps, all nine blended whatever the terrain shader;
- the map's normal decals, baked on the CPU into an overlay at one texel a
  unit when the map loads. The bake averages overlapping decals. It
  ignores a decal's turn, its blend factor, its mask and its fade. It can't
  change as a game runs.

Moho draws a screen-sized normal target first and lights everything from
it. The differences:

- The terrain's base normal is the map's own normal maps, baked by the
  editor at one texel a world unit and sampled bicubic. The engine's
  vertex normals are coarser and smoothed differently.
- Decal normals are turned, blended by their own alpha, masked and faded.
  Scripts' normal decals draw too: every structure's tarmac normals, and
  the nuke's scorch.
- Splats and decals read the terrain's normal at their own pixel. M212c's
  splats took the normal at their corners.

The rules come from:

- faf-re's `HighFidelityTerrain::DrawTerrainNormal` and `OverDrawDecals`,
  `WRenViewport::TransformTerrainNormals`, `IWldTerrainRes::GetNormalMapInfo`
  and `CWldTerrainDecal::Update` (its `TangentMatrix`);
- faf-re's `FillCubicBlendLookupTexture`;
- terrain.fx's `TTerrainNormals`, `TTerrainNormalsXP`, `TTerrainBasis`,
  `TTerrainBasisBiCubic`, `TDecalsNormals` and `TDecalsNormalsAlpha`;
- frame.fx's `BasisPS` (`TCreateBasis`).

Retail's files are read for the rules, never copied.

## The rules

### The normal target (RGBA), drawn before the scene

1. **The strata** (`TTerrainNormals`, or `TTerrainNormalsXP` for an XP
   map: the stratum material's `normals` annotation).
   - The lower stratum's normal map and the next four (eight for XP) are
     lerped by the RAW blend masks, each map ×2−1, then normalized.
   - It writes `(n·0.5+0.5).xy` into R and G alone, depth-tested.
2. **The normal decals**, in draw order: Normals (`TDecalsNormals`) and
   Alpha Normals (`TDecalsNormalsAlpha`), whose pixel shaders are the same.
   - The decal's normal is x and z from the texture's `.ag` (×2−1), with
     y = √(1−x²−z²).
   - It is turned by `TangentMatrix` = D3DX `RotationY(rotation.y)`,
     applied as `mul(M, v)`: `(c·x − s·z, y, s·x + c·z)`. Then it is
     normalized.
   - It writes `(n·0.5+0.5).xz` into R and G, SrcAlpha/InvSrcAlpha, with
     alpha `texture.r × mask.a × DecalAlpha`.
   - These are the decal passes' decals: projected on the terrain's
     triangles, faded by their LOD.
3. **The basis** (`TTerrainBasisBiCubic`: `ren_bicubicnormals` is on).
   - Each of the map's normal-map tiles is sampled at
     `(world − origin) / tileSize`. Tile i sits at
     `((i mod perRow)·w, (i div perRow)·h)`, with `perRow = mapWidth / w`:
     one texel a world unit.
   - It is sampled with a cubic B-spline, from four bilinear taps (GPU
     Gems 2, ch. 20; faf-re's 128-texel weight table holds the B-spline's
     weights).
   - Each tile's sampler clamps.
   - It writes the tile's alpha into B and its green into A.

### The composite (`BasisPS`)

From the target, `raw = texel·2−1`:

- the screen normal is `(raw.x, √(1−x²−y²), raw.y)`;
- the base normal is `(raw.z, √(1−z²−w²), raw.w)`;
- `h = normalize(base + (0,1,0))`;
- the x axis is `h.x·h·(−2, 2, −2) + (1,0,0)`, the y axis the base
  normal, and the z axis `h.z·h·(−2, 2, −2) + (0,0,1)`;
- the world normal is the screen normal's dot product with each axis.

Moho writes that to a second target. The terrain, the decals, the glow
masks and the splats then read their own pixel of it (`SampleScreen`).

## The engine

- **The map's normal maps.**
  - The scmap parser reads them: the tile width, the height and the DXT5
    blobs. Until now it skipped them.
  - `map::Terrain` keeps them.
  - A terrain without any, such as a test's own ground, gets one made from
    its heights. It takes the terrain mesh's central-difference normal at
    each unit, encoded as the map's are: x in alpha, z in green.
  - At map load the renderer puts the tiles into one texture, one texel a
    world unit. Its lookups clamp to their own tile, so a tile's edge is
    Moho's.
- **The normal pass** comes before the reflection and the scene. It uses
  the scene's render pass, on an image of the scene's format and size of
  its own, on the scene's depth, which the scene clears again. It draws:
  1. **the terrain** (`terrain_normal_frag`). The strata go to RG by the
     map's technique, and the bicubic basis to BA, in one draw. This is
     the same as Moho's two passes, since the decals write RG alone.
  2. **the normal decals** (`decal_normal_frag`): the map's and the
     scripts', with the decal pipeline's sets and push block. They blend
     into RG.
- **No second target.** `BasisPS` is a function of the pixel's own texel
  alone. The shared terrain surface GLSL applies it as it reads the
  target with `texelFetch` at `gl_FragCoord`. The terrain, the decals, the
  glow masks and the splats all read it this way.
  - The terrain's own shader no longer builds a normal.
  - The CPU overlay bake (`normal_overlay`) goes.
  - The terrain set's binding 21 holds the map's normal maps, and binding
    26 holds the target. The normal pass's shaders don't read 26, the
    image they draw.
- **Normal decals are decals.** The scenario loader keeps every type in
  `Terrain::decals()`. `DecalTechnique::Normals` names the normal pass's
  decals, and the colour passes skip them. Scripts' Normals and Alpha
  Normals draw now: a tarmac's normals, the nuke's scorch.
- **Precision.** The target is the scene's 16-bit float format, finer than
  Moho's 8 bits.

## Tests

`--terrain-normal-render-test` (gate), on ground of its own, compares
frames under the map's light and under a white fill:

1. **The base normal.** On ground rising 1 in 2 along x, the light is FA's
   formula with the normal the map's normal map gives: here, the one made
   from the heights.
2. **The strata.** A stratum normal map tilted along x, masked in over
   half the ground: the light there is `BasisPS` of its tilt, and where
   the mask is 0 it is untouched.
3. **A normal decal**, its texture tilting along its own x:
   - it tilts the light by its turn, a quarter turn tilting along z;
   - its alpha (`texture.r`) at a half blends half-way.
4. **A script's Alpha Normals decal** (a tarmac's normals) draws, and
   fades as M212c's removals do.
5. **A splat on a ridge** lights by the terrain's normal at each pixel:
   the two sides differ, which corner normals would smooth together.
6. **SCMP_009's normal map** parses: one 1024² tile.

The unit tests cover the tile layout and the normal made from the
heights.

The strata, lighting, decal, water and golden tests are re-measured: the
terrain now lights by the map's normals. The golden images change across
the terrain. They are recorded again after the change has been looked
at.

## Left for later

- `TTerrainGlow`: lava maps' terrain, the first stratum's alpha glowing.
- The water decals, and animated decal textures (M212d's list).
- The medium and low fidelity terrains.
