# M211e: FA's bloom, from the glow in the frame's alpha

The engine's bloom (M134) extracted pixels brighter than a threshold. FA's
blooms what glows, and what glows is written to the frame's alpha: a unit's
SpecTeam blue (its glow mask), the map's specular on older terrain, and,
through the map's own `Bloom`, a little of everything. Retail `frame.fx`
and `mesh.fx`/`terrain.fx`/`water2.fx`/`particle.fx` are read for their
formulas, never copied; faf-re's `CBloomRenderer` for the passes.

## The passes

`CBloomRenderer::DoBloom(amount)`, after the world and before the UI:

1. The frame, stretched into a half-size target.
2. `TCopyGlowingStuff`: `a = saturate((a - 0.02) * GlowCopyScale +
   GlowCopyAdd); c = c * a`, into a second half-size target.
3. `ren_BloomBlurCount` (2) times: a horizontal then a vertical 7-tap blur
   (offsets -3..3 texels, weights 0.1027, 0.1210, 0.1760, 0.1995, ...),
   each times `BlurScale`.
4. `TFrameAdd`: the glow added onto the frame, one to one.

| | |
|---|---|
| `GlowCopyScale` | `ren_BloomGlowCopyScale`, 2.0 |
| `BlurScale` | `ren_BloomBlurKernelScale`, 1.5 |
| `GlowCopyAdd` | the map's `Bloom` (its terrain resource's `GetBloom`; 0 without a map) |
| `MinimumGlow` | 0.02 (`frame.fx`) |

The targets are half the head's size. The blur's scale compounds: four
passes at 1.5 grow the glow's energy about fivefold.

## What each surface leaves in alpha

The scene pass clears alpha to 0, and each technique writes (or keeps) it
as FA's does:

| Surface | FA | Engine |
|---|---|---|
| Terrain, `TTerrain` | `0.01 + specular * SpecularColor.w` (`CalculateLighting`) | the same |
| Terrain, `TTerrainXP` | 0 (`TerrainAlbedoXP`) | the same |
| Units (`Unit`, `Aeon`, `Insect`, `Metal`, `Seraphim`) | `specular.b + glowMinimum` (0.01) | the same |
| Wrecks | `glowMinimum` | the same |
| Props (`NormalMappedAlpha`) | not written (`Write_RGB`) | 0 (the terrain beneath is at most 0.01 + its specular: under the copy's 0.02 for XP maps) |
| Decals, water, particles | not written (`Write_RGB`, mostly) | not written: colour write mask RGB |
| Meshes without a mesh (cubes) | — | not written |

A unit under construction (the engine's fade, until the build shaders) is
alpha-blended, which alpha can't do while it carries glow. Instances with
a fade draw after the others with a second mesh pipeline, which blends by
the fade and writes colour only; they don't glow.

## The engine

- `PipelineBuilder::set_color_write_mask`.
- The scene clears to the sky colour with alpha 0.
- Terrain and mesh shaders write FA's alpha; the mesh pipeline stops
  blending, and a fade pipeline takes the fading instances (their own draw
  groups).
- Decals, water, cubes and particles write colour only.
- The bloom's bright pass becomes the glow copy (push: `GlowCopyAdd`), the
  blur FA's kernel with its scale, run twice, and the composite a plain
  sum.
- `GlowCopyAdd` is the map's `ScmapLighting::bloom`; the B key still turns
  bloom off (FA's `ren_Bloom`).

## A trap: the instance colour interpolated

The mesh shaders passed the instance's colour on smoothly. Perspective-
correct interpolation of 1.0 across a triangle can come out a hair under
1.0, so `fragColor.a < 1.0` sent scattered pixels of finished units down
the fading branch, writing alpha ~1: full glow. The colour is per instance,
and now passes `flat`.

## Tests

`--bloom-test` (gate), on flat ground of the test's own, plates of its own
standing off it:

1. **The halo.** Around a glowing plate (SpecTeam blue 1), 7,384 pixels of
   sky brighten with the bloom on; around one that doesn't glow, none.
2. **How much.** In the dark, the bloom brightens a fully glowing plate
   6.05 times (the copy's weight 1, times the blur's growth 5.04, plus the
   frame: 6.04). One glowing a fifth, with an albedo five times brighter,
   brightens 2.90 times (alpha 0.21, weight 0.38: 2.93).
3. **Under `MinimumGlow`.** A plate that doesn't glow (alpha 0.01) isn't
   brightened (1.000). A map's bloom of 0.05 lifts it 1.148 times (1.15).
4. **No white-out.** At retail's usual bloom (0.036), the sky moves
   +0.0000 and XP terrain +0.0001, while TTerrain (alpha 0.01) is lifted
   1.062 times.
5. **Fading.** A glowing plate under construction isn't brightened
   (1.000): it blends, and writes no glow.

**Mutation testing: 12 of 12 mutants killed.**
- The copy: without `MinimumGlow`; scale 1; the map's bloom ignored.
- The blur: scale 1; once.
- The composite: at 0.3.
- Alpha: terrain at 1; XP terrain at 0.01; the sky at 1; the glow from
  SpecTeam red; the fade pipeline writing alpha; the colour interpolated.

The masks on decals, water and particles aren't exercised: the test's
scene has none.

## Left for later

- **The glow FA's decals add to alpha** (`TDecalsGlow`), with M212b.
- **Glowing particle kinds**: FA's light and beam techniques write alpha.
- The build shaders, which replace the engine's fade.
