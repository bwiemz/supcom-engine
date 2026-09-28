# M212d: Glowing decals and glow masks

## Why

A decal can make the ground glow, or stop it glowing:

- Glow decals light up the frame's bloom. A structure's tarmac lays one
  beside its albedo and normals when its tarmac has a glow texture
  (`CreateTarmac`), and maps can ship them.
- Glow Mask decals are lit colour that cuts off whatever the terrain under
  them would have glowed.

M212b and M212c kept both types but drew neither.

The rules come from:

- terrain.fx's `DecalsPSGlow`, `DecalsGlowMaskPS`, `TDecalsGlow` and
  `TDecalGlowMask`;
- faf-re's `HighFidelityTerrain::DrawNormals`, `DrawDecalPass` and
  `DrawGlowingDecals`, for the order and the gates.

Retail's files are read for the rules, never copied.

## The rules

- **The order** (`DrawNormals`):
  1. the terrain;
  2. the Glow Mask pass;
  3. the Albedo pass;
  4. the AlbedoXP pass;
  5. the splats;
  6. the Glow pass.

  Each decal pass takes its type's decals, faded by their LOD as the
  others are.
- **Glow** (`TDecalsGlow`, gated on `ren_glowingDecals`, which is on).
  - The pixel is `albedo.a × mask.r × 0.25 × DecalAlpha`.
  - It adds One/One into alpha alone. The frame's alpha is its glow, which
    the bloom reads (M211e), so the colour is left as it was.
- **Glow Mask** (`TDecalGlowMask`):
  - It is `CalculateLighting` as `DecalsPS` does it, the specular from the
    second texture's red, but without shadows (`DecalsGlowMaskPS(false)`).
  - A pixel is kept only where `saturate(albedo.a × mask.r × DecalAlpha)`
    is at least 0.9 (`clip(a − 0.90)`).
  - Where it is kept, its colour is written and the glow set to 0.01, with
    no blending.
- **The mask** (retail's `decalMask.dds`) is whole but for its border
  texel. Within a footprint it is 1.

## The engine

- `renderer::DecalTechnique` (`decal_math`) names the four passes, and
  `decal_technique(type)` gives a type's technique. It is none for the
  normal and water types. It replaces M212b's `map::lit_decal`.
- The map's decals and the scripts' both draw through it: a stored decal,
  and a runtime decal's draw, carry their technique.
- The two new pipelines take the decal pipeline's sets and push block, with
  layouts of their own that match it:
  - `decal_glow_frag` blends One/One into alpha;
  - `decal_glow_mask_frag` writes colour and glow, without blending.

  Both run over the shared terrain surface GLSL.
- The decal pass becomes one pass a technique, in Moho's order, with the
  splats between AlbedoXP and Glow.
- **For tests,** `Renderer::request_scene_capture` reads back the scene
  image before the bloom and the UI. It is the colour as floats, and the
  glow in alpha, which the swapchain capture can't show.

## Tests

`--decal-render-test` (gate) gains two checks:

9. **A Glow decal** adds 0.25 to the glow and leaves the colour as it was.
10. **A Glow Mask decal:**
    - it draws its lit colour where its alpha is 1, and sets the glow to
      0.01;
    - at half alpha it draws nothing;
    - a Glow decal over it still adds its 0.25, which pins the order.

Test 7 now takes a Water Mask decal for a type that draws nothing here,
since a Glow Mask decal draws.

`--runtime-decal-test` (gate) gains Test 11: a script's Glow decal adds
0.25 too.

The unit test of the decal types checks each type's technique.

## Left for later

- **Runtime Normals and Alpha Normals decals** (M212e). A tarmac's
  normals and the nuke scorch draw into the terrain's normals. The engine
  bakes the map's into a CPU overlay at map load, one texel a unit, and
  binds it once. Scripts' ones come and go, and fade, so they want the
  overlay drawn on the GPU, at a finer scale.
- **The water decals** (Water Albedo, Water Mask, Water Normals). SCMP_009's
  decals are Albedo and Normals only. Campaign objectives make Water Albedo
  ones (`SimObjectives.lua`).
- **Animated decal textures** (`CAnimTexture`).
