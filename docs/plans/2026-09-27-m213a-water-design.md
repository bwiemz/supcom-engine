# M213a: FA's water surface

## Why

The engine's water was invented:

- a tessellated grid with three sine waves;
- shallow, mid and deep colours picked by hand;
- a made-up specular term.

None of it read the map's water. FA draws water with water2.fx's
`Water_HighFidelity` technique, over one quad that covers the map. It uses:

- the map's water parameters;
- four scrolling wave normal maps;
- a Fresnel table;
- the sky cubemap;
- a refraction of the frame drawn so far;
- a reflection of units.

The rules come from:

- water2.fx: `WaterVS`, `HighFidelityPS`, `TWaterLayAlphaMask`;
- faf-re: `HighFidelityWater` (`InitVerts`, `RenderWaterSurface`,
  `BuildFresnelLookupTexture`), `CWldMap`'s `RebuildWaterMapRect` (the water
  map), and `CWorldView`'s pass order.

Retail's files are read for the rules, never copied.

## The rules

### The quad

The quad runs (0, 0)–(width, height) at the water's elevation, with UV 0–1
across it.

### The water map

The water map (UtilityTextureC) is half the map's size. Each texel covers two
heightmap units, and its channels are:

| Channel | Holds |
| --- | --- |
| R | The map's flatness mask (default 255). |
| G | The depth `(water − height)/(water − abyss)` × 255, clamped to 0–255, where any of the texel's four corners is under the water. |
| B | 255 where any corner is above the water. |
| A | The map's foam mask (default 0). |

The masks follow the water parameters in the `.scmap`: foam, flatness, then
depth bias. Depth bias is not used by the high-fidelity water.

### The Fresnel table

The table is 128×128. The row is the incidence i (NdotV); the column is the
depth d.

- **Fresnel term:** `f = clamp(d·bias + (1 − d·bias)·(1 − i)^power, 0, 1)`.
- **Second channel:** `f·i^shininess·sunReflection`, which the pixel shader
  doesn't read.

### The surface (`HighFidelityPS`)

1. **Normal.** Four wave normal layers are sampled at
   `(xz + movement_k·time) · repeat_k`, with time in ticks.
   - Summed, the normal is `N = normalize((2·sum.xyz − 4).xzy)`. It is
     lerped from up by flatness (R).
   - The wave crest is `saturate(sum.a − 1)`.
2. **Refraction.** The frame drawn so far is read at the screen position,
   offset by `−refractionScale·N.xz/w`.
   - The background is the unrefracted read. Where the refracted read's alpha
     is not 0, the background is used instead.
3. **Reflection.** The sky cubemap is read along `reflect(view, N)`. Units'
   reflections are lerped in by `unitReflection` (none yet: M213b).
4. **Colour.**
   - **Water colour:** the map's `surfaceColor`, lerped in by the depth
     clamped to `colorLerp`.
   - **Fresnel:** `fresnel(depth, NdotV)`.
   - **Sky reflection:** its amount is the map's `skyReflection` ×
     saturate(10·depth), lerped in by that amount × the Fresnel.
   - **Sun glint:** `pow(saturate(dot(−R, sunDirection)), sunShininess) ·
     sunColor·sunReflection·fresnel`.
   - **Crest:** white, lerped in by `(1 − foam)·crest`.
5. **Output.**
   - RGB only, with no blend.
   - A pixel whose frame alpha marks it as not water (mask ≠ 0) is dropped.
   - Depth test LessEqual, with no depth write.

### The alpha mask (`TWaterLayAlphaMask`)

Before the frame is copied, the quad writes alpha 0 wherever the water map's
B is 0 (open water). It writes alpha only, depth-tested. Elsewhere the frame
keeps its alpha (its glow, M211e).

The copy's alpha is then 0 over water, and the water draws no glow.

`TTerrainXP` terrain writes alpha 0 everywhere (`TerrainAlbedoXP` returns
it), so on XP maps the surface can't tell a shore texel's land from open
water and draws over both. It still never covers dry land, which lies in
front of it: the surface is depth-tested. This is FA's behaviour, kept.

### Under the water (`ApplyWaterColor`)

terrain.fx tints the terrain under the water after lighting it, in both
`TTerrain` (`CalculateLighting`) and `TTerrainXP`. It reads the water map's G
(the depth) at the terrain's map position, then the map's water ramp
(`waterramp.dds`) at that depth, and lerps the lit colour to the ramp's RGB by
the ramp's alpha. Both samplers are linear and clamped. The ramp's alpha is 0
at depth 0 and nearly 1 in deep water, so shallows show their ground and deep
water reads the ramp's navy through the surface.

Without this the refraction shows the lit sea floor, and deep water looks grey.

### Pass order

The passes run as in `CWorldView`:

1. Terrain, decals, meshes, and effects under the water.
2. The alpha mask.
3. The copy of the frame (refraction).
4. The water.
5. Beams, particles and trails.

## The engine

- **Map data.**
  - `ScmapWater` gains the four normal repeat rates and the four wave
    textures (movement and path).
  - `ScmapData` gains the three masks.
  - `Terrain::set_water` carries them, with the abyss elevation.
- **`WaterRenderer`.** It is rebuilt around the quad. It builds:
  - the water map and the Fresnel table (both RGBA8);
  - the wave textures and the cubemap;
  - a uniform buffer for the parameters, a set per frame, and two pipelines
    (mask, surface).
- **`Renderer`.** On a map with water, the scene pass is split around the
  water:
  - the first pass stores its depth;
  - the frame is copied to a refraction image;
  - a second pass loads colour and depth.

  The pipelines stay as they are, since the passes are compatible. A map
  without water keeps the single pass.

## Tests

`--water-render-test` (gate), SCMP_009:

1. **The water map.** Depth and the land flag are checked at texels on open
   water, on the shore and on land.
2. **The masks** read in order: flatness full and foam none over most of the
   map.
3. **The Fresnel table** against the formula, head-on, at no incidence and
   between.
4. **Under the water.** Deep water, seen through the surface, is navy and
   dark; shallow water is brighter, showing its ground.
5. **Refraction.** A red glow hung under the surface shows through it.
6. **Scrolling.** Ten ticks on, the open water has changed; dry land, looked
   at from above it, hasn't.
7. **The map's parameters.** The same view is drawn again at the same tick
   with one change at a time. A red water colour (lerped fully) turns the
   water red. No sky reflection, no sun reflection, or foam everywhere (no
   crests) each darken it. A redraw with nothing changed is identical, so
   even the crests' few pixels register.

## Left for later

- Units' reflections (M213b).
- The shoreline and its wave sprites (`WaveSystem`).
- The low-fidelity water.
- REFRACT particles (M214d), which will read the same copy.
- Decals' water tint: FA lights decals and tints them as it does the terrain
  (`DecalsPS`), but the engine's decals are not lit yet (M212).
