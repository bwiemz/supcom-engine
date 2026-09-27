# M210a: the map's lighting

Every FA map carries its own light: a sun (direction and colour), ambience,
a shadow fill colour, a lighting multiplier, specular and bloom. Evergreen
maps are cool, and tropical maps warm and amber. The renderer ignored all of
it. The parser skipped the 92-byte block, and every lit shader used the same
invented lighting:
- a fixed sun direction of (0.5, 1, 0.3);
- a fill light from the other side;
- hemisphere ambient;
- Blinn-Phong specular;
- an exponential blue haze.

So every map looked alike.

The reference is FA's own shaders, which retail ships as source in
`gamedata/effects.scd`. The shaders are read for their formulas only and
never copied into the repository:
- `terrain.fx`: `CalculateLighting` (the `TTerrain` technique) and
  `TerrainAlbedoXP` (`TTerrainXP`);
- `mesh.fx`: `ComputeLight`.

## The map's data

The block follows the environment cubemaps: 23 floats.

| Field | Floats | SCMP_009 | SCMP_010 (tropical) |
|---|---|---|---|
| LightingMultiplier | 1 | 1.54 | 1.43 |
| SunDirection | 3 | (0.62, 0.56, 0.56) | (0.50, 0.50, 0.71) |
| SunAmbience | 3 | (0, 0, 0) | (0, 0, 0) |
| SunColor | 3 | (1.38, 1.29, 1.14) | (1.83, 1.29, 0.95) |
| ShadowFillColor | 3 | (0.54, 0.54, 0.70) | (0.64, 0.61, 0.45) |
| SpecularColor | 4 | (0.31, 0, 0, 0) | (0.37, 0, 0, 0) |
| Bloom | 1 | 0.036 | 0.076 |
| FogColor | 3 | (0.37, 0.49, 0.45) | (0.28, 0.22, 0.17) |
| FogStart, FogEnd | 2 | 0, 740 | 0, 700 |

Checks against four retail maps: every sun direction has unit length, and
the multipliers run 1.2–1.54.

Before the block come:
- the terrain shader: `TTerrain` on the original maps, `TTerrainXP` on the
  expansion's;
- the background texture;
- the sky cubemap;
- the environment cubemaps by faction.

After it, the water block carries its own sun and colours. The parser reads
all of these, and M213 (water) and the sky dome use them later.

## FA's lighting

Terrain and units light the same way (`ComputeLight`, `CalculateLighting`):

```
light = SunColor * saturate(dot(SunDirection, N)) * shadow + SunAmbience
light = LightingMultiplier * light + ShadowFillColor * (1 - light)
```

So a surface facing away from the sun, or in shadow, takes the shadow fill
colour, and a lit one is pushed past 1 by the multiplier (FA lights in gamma
space; the swapchain is UNORM). `SunDirection` points toward the sun.

**Terrain specular** depends on the map's terrain shader:
- **`TTerrain`:** `pow(saturate(dot(R, V)), 80) * SpecularColor.x * (1 -
  albedo.a)`. Here `R = S - 2 (S·N) N` and `V` runs from the eye to the
  point. It is added into `light` before the multiplier, and the colour is
  `light * albedo`.
- **`TTerrainXP`:** `pow(saturate(dot(reflect(V, N), S)), 80) * albedo.a *
  SpecularColor.a * SpecularColor.rgb`. The colour is `light * (albedo +
  specular)`.

**Fog.** No FA shader applies distance fog, and none sets fixed-function
fog states. The map's fog values go unused in game. The engine's haze was
its own invention, so it goes.

**Bloom** is left for M211e. FA's is a glow pass: `frame.fx` copies what
glows out of the frame's alpha with `GlowCopyScale` and `GlowCopyAdd`,
which the engine sets. How the map's `Bloom` value feeds them isn't in the
shaders. The engine's bloom stays as it is, and the value is parsed for
later.

## The engine

- **`ScmapData`** gains `lighting` and `environment` (the shader, textures
  and cubemaps).
- **The light UBO**, which the lit shaders already bind for the shadow
  matrix, gains the sun direction, sun colour, ambience, shadow fill,
  multiplier, specular colour and the terrain shader's variant. The
  lighting is written when a scene is built; the matrix is written each
  frame.
- **The terrain and mesh shaders** use FA's formulas. Units have no
  material system yet (M211), so they keep their current texture sampling
  and only change their lighting.
- **The shadow map** looks along the map's sun direction
  (`math::light_view_proj`). For a sun overhead, +Y is its view direction
  and can't be its up: the view collapsed the whole map onto one texel.
  It takes +Z then, as Moho's `Shadow::PrepareLightCamera` falls back to
  a fixed up when the light lines up with its reference.
- **The sun direction is used as the map stores it.** Moho hands
  `GetSunDirection()` to the shaders unnormalized; retail maps' have unit
  length. Only the shadow map's view normalizes it.
- **Without a map** (the front end, tests with no map) the light is a
  neutral default: SCMP_009's.

## What FA's formula exposed

The invented lighting had a strong ambient term, which hid three renderer
bugs. Under FA's formula, where a surface's light comes almost entirely
from `N·S`, they showed:

- **Stratum normal maps were decoded as DXT5nm** (x in green, y in alpha).
  FA's `TerrainNormalsPS` reads them as ordinary RGB normals, z up
  (`tex * 2 - 1`), whatever their compression. SCMP_009's are DXT1, 3
  and 5, and every one is bluish RGB. With alpha at 1 the DXT5nm decode
  laid each normal almost flat, so flat ground took a quarter of the sun
  or less, and none from overhead. Only decal normals are DXT5nm; mesh
  normals are too (`mesh.fx` reads `.gaa`). The flat-normal fallback is
  now (128, 128, 255, 128), flat in both encodings.
- **The strata textures were fetched with the async `get()`** when the
  scene was built, and the terrain's descriptor set is written once then.
  On a first load nothing was cached yet, so the terrain kept the white
  fallback all game. That was SCMP_009's washed-out white "snow". They are
  now loaded blocking, as the other init paths are.
- **With bloom off from the first frame**, the composite pass sampled the
  bloom image before any bloom pass had written it: an UNDEFINED layout
  (a validation error) with undefined contents. A NaN there survives the
  zero strength. The scene image stands in when there is no bloom.

The shadow map's view also degenerated for an overhead sun, whose view
direction is its up vector; it now takes +Z as up there.

## Tests

- **Unit test:** the lighting block and strings are read from a synthetic
  map. The fields land in order, and the sections after it (water, strata,
  props) still parse.
- **Data test:** SCMP_009's values, as the table above gives them, and the
  terrain shader `TTerrain`.
- **Render test** (`--lighting-test`, offscreen): one view of flat open
  land, drawn under different lighting and compared per pixel (medians,
  so the few props in view don't decide it):
  - Under each map's own light, flat ground takes FA's light: its frame
    over a white fill's (the albedo alone) is the formula's value, to
    0.05. SCMP_009's is drawn first, on the scene's first build.
  - SCMP_010's tropical sun makes a redder frame than SCMP_009's
    evergreen one.
  - With no sun, the ground takes the shadow fill colour: a (0.5, 0.25, 1)
    fill scales a white fill's frame by exactly that.
  - The multiplier scales the sunlight, with the sun overhead.
  - A sun 0.8 high lights flat ground 0.8 as much as one overhead.
  - The terrain shader picks the formula: under a glint (the sun where
    the view reflects, a full specular colour), `TTerrain`'s frame and
    `TTerrainXP`'s differ.
  - No Vulkan validation error (Debug builds run the layers).
- **Unit test** (`[shadow]`): the shadow map's view centres on its
  target and looks down the sun; an overhead sun still spreads the ground
  across it; the direction's length doesn't matter.
  
  Measured against the formula, flat ground's light is (1.30, 1.25, 1.23)
  under SCMP_009's (predicted 1.31, 1.26, 1.235) and (1.36, 1.14, 0.92)
  under SCMP_010's (predicted 1.36, 1.14, 0.915).
- The existing render tests are checked, not loosened: those that assumed
  the old haze or ambience are updated to FA's formulas.
