# M211f: FA's build shaders

A unit under construction drew as the engine's stand-in: its finished look at
40% opacity. FA draws it with its faction's build technique: UEF units pulse
blue under a scrolling overlay, Aeon ones grow from 75% under a rippling
silver sheen, Cybran ones fade in under red scan lines, Seraphim ones grow
from a quarter size through a shimmering distortion. Retail `mesh.fx` is read
for the formulas, never copied; faf-re's `MeshRenderer` and `UserEntity` for
the inputs; retail Lua for when a unit wears the build mesh.

## When a unit wears it

Retail `Blueprints.lua` (`ExtractBuildMeshBlueprint`) gives every UEF, Aeon,
Cybran and Seraphim unit a second mesh blueprint, `<mesh>_build`: a copy of
its own whose LODs have `ShaderName = '<Faction>Build'`, `SecondaryName =
'/textures/effects/<Faction>BuildSpecular.dds'` and, for Seraphim, the
falloff `LookupName`. `Unit.StartBeingBuiltEffects` (`/lua/sim/Unit.lua`)
`SetMesh`es it when construction starts; `StopBeingBuiltEffects` puts the
unit's own mesh back. The engine already draws a `SetMesh` override.

Moho has no generic fade for a unit under construction: its mesh's technique
decides. The engine's 0.4 alpha for `fraction_complete < 1` goes; a unit
under construction without a build mesh (other factions, mods) draws as
itself, as in FA. The build ghost keeps its fade.

## The inputs

| FA | Engine |
|---|---|
| `material.y`, the technique's `parameter` annotation: `PARAM_FRACTIONCOMPLETE` for the four | `MeshInstance::parameter`, the entity's `fraction_complete` (UserEntity copies it at each sync, not interpolated) |
| `vertex.material.x = time - material.x`: the instance's age, in ticks | `pc.time - shader_time` |
| `time`: `fmod(gameTick + deltaFrame, 36000)` (`MeshRenderer::ConfigureShader`) | the newest tick plus the frame's interpolant, wrapped, pushed per draw |
| `secondarySampler`: the LOD's `SecondaryName`, linear, wrapped | `GPUMesh::secondary_path`, set 6 |
| `texcoord0.zw`: the second UV set | the first: all 638 retail unit LOD0 meshes have the two identical |

## The techniques (High fidelity; Seraphim's highest is Medium)

Shared: `teamColor = color * ((f >= 0.9) ? (f - 0.9) * 10 : 0)`, the team's
colour fading in over the last tenth, lerped in by SpecTeam alpha. `f` is the
fraction complete, `age` the instance's age, `S` the sun, `V` FA's view
direction. None names an `environment`: all reflect the map's `<default>`.

**UEFBuild.** P0 `NormalMappedVS` + `UEFBuildHiFiPS`, blended, colour only:
`color` as `NormalMappedPS` but a highlight of `pow(., 8) * spec.g`; then
`secondary = tex(uv * 50 + (0, 0.62 age))`, `t = clamp(frac(0.02 time), 0.35,
0.7)`, `out = lerp(lerp(color + secondary, blue, t), color, f)`, alpha
`max(f, 0.5)`. P1 `EffectVertexNormalHiFiVS(16, 8, 0.0192, 0.0176, -0.0122,
-0.0122)` + `UEFBuildOverlayHiFiPS`, blended, **colour and alpha**:
`x = tex(uv * 16 + age (0.0192, 0.0176))`, `y = tex(uv * 8 - 0.0244 age)`
(FA adds both shifts to both of `zw`), `rgb = x + y`, `a = max((x.a + y.a)
(1 - f), 0.25)`, times `1 - (f - 0.95) * 20` past 95%.

**AeonBuild.** Both passes `AeonBuildVS`: the mesh scaled by `max(f, 0.75)`.
P0 `AeonBuildPS`, **opaque**, colour only: `AeonPS`'s light (the sun at 0.6),
`pow(., 8) * spec.g`, `spec.r * env`, the team fade. P1 `AeonBuildOverlayPS`,
blended, colour only: two scrolling reads of the secondary make a grey sheen
(`m1.r - m2.g + m1.g m2.r`, lerped 0.75 to 0.5), which bends the normal
(`lerp(lerp(normal.gaa, secondary(uv * 7).baa, 0.5), sheen, 0.5)`); `rgb =
sheen * sat(N.S) + pow(sat(reflect.V), 8)`, alpha `rgb.r * 2`, faded past 95%.
No shadow: the technique has no depth stage.

**CybranBuild.** P0 `NormalMappedVS` + `CybranBuildPS`, blended, colour only:
`NormalMappedInsectPS` plus the team fade; alpha 0.4, rising to 1 from 70%.
P1 `EffectVertexNormalLoFiVS(14, 4, 0, 0, -0.008, 0.008)` +
`CybranBuildOverlayPS`, blended, colour and alpha: `s = tex(uv * 14)`, `m =
tex(uv * 4 + age (-0.008, 0.008))`, `(0.75 s.a, 0, 0, m.r s.a)`, faded past
95%.

**SeraphimBuild.** `SeraphimBuildVS`: the mesh scaled by `0.25 + 0.75 f`.
`SeraphimBuildPS`, blended, colour only: the secondary at `(uv + (0, 0.005
age)) * 0.5`, times 0.03, offsets the albedo, SpecTeam and normal reads by
its `rb`, times `1 - (f - 0.9) * 10` (10 at f = 0, none when built); then
`UnitFalloffPS`'s colour (falloff by `N.V` at the army's row, no sun) and
alpha `max(f, 0.25)`. Its shadow is scaled too (`SeraphimBuildDepth`).

## Render states

`d3d9states.compat`: `Write_RGB` is `ColorWriteEnable 0x07`; FA sets no
separate alpha blend, so the overlays that write alpha blend it by the same
`SrcAlpha, InvSrcAlpha`. Depth is left at the device's (test and write,
less-equal), so an overlay passes over its own base pass.

| Pass | Engine pipeline |
|---|---|
| UEF, Cybran, Seraphim P0; Aeon P1 | the fade pipeline (blend, colour only) |
| Aeon P0 | the fade pipeline with alpha 1 (opaque, colour only) |
| UEF, Cybran P1 | a new overlay pipeline: colour and alpha blended by source alpha |

Groups drawn with a build technique come after the opaque ones, P1 straight
after P0, as FA draws a batch's passes.

## Shadows

The shadow pass skips `AeonBuild` groups and scales `SeraphimBuild`
vertices by `0.25 + 0.75 f` (a per-draw technique and the instance's
parameter); `UEFBuild` and `CybranBuild` cast as the unit.

## Tests

`--build-shader-test` (gate). Real units for the wiring, plates of the test's
own (off its ground, over the sky's clear colour) for the formulas:

1. **Construction.** An engineer of each faction builds its T1 power
   generator: mid-build it wears `Display.BuildMeshBlueprint`, drawn by
   `<Faction>Build`; built, its own mesh and technique.
2. **UEF's pulse.** Black albedo and secondary: `(0, 0, t)` blended at 0.5,
   under the overlay's floor of 0.25, for `t` at 0.35 and 0.7; at 80%, a
   fifth of it at 0.8.
3. **UEF's overlay.** A red secondary: its alpha, and past 95% its fade.
4. **Aeon.** Grows from 75%; opaque; its overlay's highlight, and the sheen
   under a low sun.
5. **Cybran.** 0.4 until 70%, then rising; the red lines, fading past 95%.
6. **Seraphim.** Grows from 25%; the falloff at the army's row; alpha
   `max(f, 0.25)`; the distortion shifts a striped albedo.
7. **Team colour.** UEF, Aeon and Cybran: none at 85%, 0.6 of it at 96%.
8. **Age.** Each technique's overlay or distortion moves between frames
   ticks apart; a finished unit's doesn't. A plate, and another made in its
   place ticks later, each looked at as it's made, look alike: by age, not
   the clock.
9. **Shadows.** Under a hovering build plate: a UEF one's full shadow, a
   Seraphim one's scaled, an Aeon one's none.

The bloom test's fifth check changes with this: a plate under construction
is a Seraphim one (colour only: no glow), and a UEF one glows by its
overlay's alpha, blended as D3D9 blends it (a² over the sky's 0).

## Left for later

- **Hidden bones.** The renderer ignores `HideBone`: retail hides a UEF
  structure's bone 0 until its build cube has risen, and every ACU's
  enhancement parts until they're built.
- **The build effects' own meshes**, projectiles: `UEFBuildCube` (the cube
  a UEF structure rises from), `AlphaFade` and `TMeshGlow` (its slices and
  flash), `AeonBuildPuddle`. Unported, they draw as opaque boxes; a UEF
  structure under construction sits inside one (M211g).
- **Burnt trees**: `BlackenedNormalMappedAlpha` is its own technique (a
  greyed albedo, lit, alpha-tested by fraction), not the wreck path M211d
  sent it down.
- `ComputeScrolledTexcoord` (`anim.w`), for scrolling meshes.
