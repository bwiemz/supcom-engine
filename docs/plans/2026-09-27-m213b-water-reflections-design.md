# M213b: Units reflected in the water, and meshes drawn after it

## Why

M213a drew FA's water surface, but two things around it are still the
engine's own.

- **The reflection.** The surface lerps a reflection of units into its sky
  reflection by the map's `unitReflection`. The engine had no reflection, so
  the water showed only the sky.
- **The order of meshes.** Every mesh was drawn before the water. Moho draws
  some techniques after it (rocks, trees, the build slices). The water writes
  no depth, so those show over it, unrefracted and untinted.

The rules come from:

- faf-re's `WRenViewport::Render` and `RenderReflections`
  (`app/WxRuntimeTypes.cpp`);
- `MeshRenderer::Render` and `ConfigureShader` (`mesh/Mesh.cpp`);
- `MeshBatch::Render` and `FillBatch` (`HardwareMeshBatch.cpp`);
- `UserEntity::CreateMeshInstance` and `UserUnit::CreateMeshInstance`;
- mesh.fx's `renderStage` annotations and its `mirrored` branches.

Retail's files are read for the rules, never copied.

## The rules

### Render stages

Each mesh.fx technique carries a `renderStage` annotation, a set of these
bits:

| Bit | Value |
| --- | --- |
| DEPTH | 0x01 |
| REFLECTION | 0x02 |
| PREEFFECT | 0x04 |
| POSTEFFECT | 0x08 |
| PREWATER | 0x10 |
| POSTWATER | 0x20 |

`Render` draws meshes in four buckets, in this order:

1. 0x14: pre-water, pre-effect.
2. 0x18: pre-water, post-effect.
3. The water.
4. 0x24 and 0x28, the post-water buckets, the effects between them.

The post-water techniques the engine draws are `AlphaFade`,
`BlackenedNormalMappedAlpha`, `VertexNormal` and
`UndulatingNormalMappedAlpha`. Every other ported technique is pre-water:
the units' and factions', the build shaders, `NormalMappedAlpha`,
`NormalMappedGlow`, `UEFBuildCube`, `AeonBuildPuddle`,
`NormalMappedTerrain` and wrecks.

### The reflection pass (`RenderReflections`)

The reflection is drawn before the scene, into a target of its own, cleared
to transparent black.

- **What is drawn.** `Render(2)` passes every technique through the stage
  filter. `FillBatch` then keeps only instances whose `isReflected` is set.
  - A mesh instance is made reflected.
  - `UserEntity::CreateMeshInstance` clears the flag. That covers props,
    wrecks, projectiles and recon blips.
  - `UserUnit::CreateMeshInstance` doesn't clear it.
  - So only units are reflected.
- **The mirror** (`ConfigureShader` with `mirrored`).
  - The view is reflected about the water plane: `r[3] += 2·surfaceElevation·r[1]`, `r[1] = −r[1]`. That is the world seen through `y → 2e − y`.
  - The sun direction is negated.
  - No shadow is bound.
  - Meshes keep their techniques' culling. Their effects set `CullMode`
    statically and nothing flips it, so a reflection shows its unit's far
    side.
- **mesh.fx when `mirrored`.**
  - `clip(position.y − surfaceElevation)`: nothing under the water is
    reflected.
  - Alpha is 0.5.
- **In the water.** The surface reads the reflection at its refracted screen
  position, and lerps it over the sky by `saturate(unitReflection · alpha)`.
  With the default `unitReflection` of 0.5, a unit reflects at a quarter.

## The engine

- **Stages.**
  - `mesh_cache.hpp` gains `is_post_water_technique`.
  - The renderer's mesh draw loop becomes `draw_meshes(pass)`. The pass is
    one of: all, before the water, after the water, or the reflection.
  - On a map with water, the scene draws the pre-water meshes in its first
    pass, and the post-water ones in its second, after the surface and
    before the effects.
- **Groups.** `UnitRenderer` groups instances by mesh, blend and whether they
  are reflected. Reflected means a unit that is neither a wreck nor a
  remembered structure.
- **Reflection target.**
  - `reflection_image_` is in the scene's colour format and is made with the
    other frame targets.
  - Its framebuffer pairs it with the scene's depth image. The scene pass
    clears that depth again afterwards.
  - It is drawn with `scene_render_pass_` itself, so every mesh pipeline
    draws into it.
  - Each frame on a map with water, the reflected groups are drawn into it
    with the mirrored view-projection. `mesh_frag` derives FA's view
    direction from the view-projection (`faViewDirection`), so the
    reflection is seen from the eye mirrored, as in Moho.
- **Mesh shaders.**
  - The push block gains `mirrored` and the water's elevation (104 bytes).
  - When mirrored, `mesh_frag` negates the sun, drops the shadow, discards
    below the water, and writes alpha 0.5.
- **Water.** `WaterRenderer::set_reflection` binds the target, where M213a
  bound a transparent placeholder.

## Tests

`--water-reflection-test` (gate), SCMP_009:

1. **After the water.** A post-water prop stands in shallow water.
   Recolouring the water red leaves the prop's submerged pixels unchanged. A
   pre-water prop in the same place turns red.
2. **A unit reflects.** A unit's wall stands on open water. With the
   reflection on against off, the water changes where its mirror image shows,
   below it on screen, and not beside it.
3. **Only units.** A floating prop in the same place leaves the water
   unchanged.
4. **Nothing under the water.** A unit sunk under the surface reflects
   nothing.
5. **At half alpha.** `unitReflection` 1 lerps the reflection in by half, and
   2 does so wholly. 4 does no more than 2.
6. **The sun negated, no shadow.** A low sun behind a wall leaves its face
   dark. Its reflection, lit by the sun turned about, is lit, though a wall
   behind it shades it from that sun.
7. **From the eye mirrored.** In the dark, a wall that reflects its whole
   environment (white above, black below) shows black. Its reflection, seen
   from under the water, shows white.

The walls are the test's own meshes, standing in for units and props. The
reflection is boosted (`unitReflection` 4, `skyReflection` 10), so the water
shows it whole. Each comparison is two frames at the same tick.

## Left for later

- The shoreline's wave sprites (`WaveSystem`).
- The low-fidelity water.
- Effects' pre- and post-effect mesh buckets (`Clutter`, not yet ported).
- Decals' water tint (M212).
