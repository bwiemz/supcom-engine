# M212a: the terrain's strata, as FA blends them

M210a lit the terrain as FA does. What the light falls on is still the
engine's own blend of the map's strata, and it differs from FA's in four
ways. The reference is retail `terrain.fx` (read for its formulas, never
copied): `TerrainPS` (the `TTerrain` technique), `TerrainAlbedoXP`
(`TTerrainXP`) and `TerrainNormalsPS` / `TerrainNormalsXP`. Moho's
`CStratumMaterial` (faf-re) confirms the tiling: each stratum's UV is its
world position over the stratum's size, as the engine already has it.

## FA's blend

A map has ten strata: the lower (the scmap's stratum 0), eight blended
ones (1-8, FA's `Stratum0`-`Stratum7`), and the upper (9). Two blend
textures hold the eight masks, four channels each.

- **The albedo masks are sharpened**: `mask = saturate(tex * 2 - 1)`. A
  mask under one half adds nothing, and at one it replaces the layer
  below. The engine blended by the raw mask, so every stratum bled into
  the ground below it at half strength.
- **Normals blend by the raw mask** (`TerrainNormalsPS` reads the blend
  texture as it is).
- **`TTerrain` maps blend four strata** (1-4, the first blend texture).
  Only `TTerrainXP` maps use the second texture and strata 5-8. The engine
  blended all eight on every map; SCMP_009 keeps a copy of its first blend
  texture as the second, which the engine had worked around (M183) by
  giving texture-less strata no weight.
- **The upper stratum** is laid over the rest by its own alpha:
  `albedo.rgb = lerp(albedo.rgb, upper.rgb, upper.a)`, tiled by its own
  size (a macro texture, 128 on many maps). The engine ignored it. It has
  no normal map.

The specular mask (`albedo.a`) blends with the sharpened masks too; the
upper stratum leaves it alone.

- **Normal maps have their own tiles** (`StratumNNormalTile`). The engine
  tiled them by the albedo's size, for want of push-constant room, and on
  SCMP_009 they differ widely: stratum 0 is albedo 4 and normal 8.75,
  stratum 3 albedo 16.8 and normal 2.

Both maps checked put `macrotexture000` in the upper stratum (SCMP_009 at
128, SCMP_010 at 69.5).

## The engine

- **The strata's sizes** (nine albedo, the upper's, nine normal) move from
  the push constants to a uniform buffer in the terrain's descriptor set
  (binding 23). The push block had grown to 140 bytes, past the 128 that
  Vulkan guarantees; it is 92 now.
- **The upper stratum's albedo** is bound at binding 22. Without one, a
  transparent texel leaves the strata below as they are.
- **The terrain shader variant** (M210a's flag in the light UBO) picks four
  strata or eight: on a `TTerrain` map the second blend texture is not
  read.
- **The albedo blend sharpens its masks**; the normal blend does not.
- The M183 workaround stays: strata without a texture get no weight. On a
  `TTerrain` map it no longer matters, since the second texture isn't read.

## Results

- **SCMP_009** now shows green grassland and brown slopes where the raw
  masks had spread half-strength strata everywhere. It matches FA's own
  lobby preview (the `.scmap`'s preview image).
- **SCMP_010**'s interior is olive jungle with sand beaches, as in FA's
  preview, once the engine's fog-of-war dimming is off (`--no-fog`). The
  dimming of unexplored ground at the start is the engine's own and
  belongs to M215.

## Tests

`--strata-test` (gate) draws a flat 64 × 64 terrain of its own offscreen:
game textures as strata, and uniform blend textures it writes as the maps
do (uncompressed BGRA DDS). Lit by a white shadow fill and no sun, a frame
is the blended albedo alone.
1. A mask of one half changes the ground by 0.0009 a channel; a full mask
   by 0.23, to exactly the stratum alone at its own size.
2. A mask of three quarters (0.498 once sharpened) gives that mix, to 0.001.
3. On a `TTerrain` map a stratum of the second texture changes nothing; on
   a `TTerrainXP` map it is all that shows.
4. An opaque upper stratum (DXT1) covers the ground, at its own size.
5. Under a sun, a normal map at size 16 rather than 4 changes the lit
   ground: it repeats at its own size, not its albedo's.
6. Normals blend by the raw mask: at one half, a stratum without a normal
   map flattens half the relief below, though its albedo adds nothing.

`--lighting-test`'s helpers for capturing and comparing offscreen frames
move to `render_probe.{hpp,cpp}`, which both tests use.
