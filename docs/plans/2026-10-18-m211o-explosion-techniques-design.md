# M211o: the explosions' effect techniques

The UEF nuke drew as an opaque, lit sphere with a pinched highlight at its
pole. Its dome (`UEFNukeEffect01`, the `Sphere01` mesh) names `ShaderName =
'NukeUEF'`, which `mesh_technique` didn't know, so it fell back to `Unit`:
opaque, lit by `NormalMappedPS`. `Effect` (`TMeshAdd`), `Cloud`
(`TMeshCloud`) and `OuterCloud` (`TMeshOuterCloud`) fell back the same way.
Retail `mesh.fx` is read for the formulas and states, never copied.

In retail's data the UEF nuke draws one mesh, the dome; its controller's
other children are emitters. `OuterCloud` draws the EMP warhead's cloud
(`EMPFluxWarheadEffect01`/`02`), `Effect` the teleport's shockwave
(`/effects/Nuke/Shockwave01`, made by `UnitTeleport01`-`03`), and `Cloud`
`/effects/Nuke/Plume01`, which no retail script makes.

## The techniques

All four take `EffectVS`: the vertex placed as a unit's, its texture
coordinate, and `material.x` as the instance's birth, so `age` is the
instance's age in ticks (as M211f's). `EffectVS` never sets the normal: it
is 0. Only `Cloud` has fidelity lanes (High, Medium and Low); they differ in
`EffectFadePS`'s `hiDefShadows` argument, which its body never reads, so the
three light it alike and the engine draws one.

| Technique | Pixel shader | Render state | Stage |
|---|---|---|---|
| `Effect` | `EffectPS`: the albedo at `(u, (v + 0.0008 age) * 3)`, unlit; alpha `(1 - albedo.a at uv) * fade(180, 0.025)` | no blend or depth state named (see below): opaque, colour and alpha, depth written | after the effects |
| `Cloud` | `EffectFadePS(180, 0.025)`: the albedo times `ComputeLight(dot(sun, 0), 1)`, the ambience alone: `ambience + (1 - ambience) * fill`, times the light multiplier; alpha `albedo.a * material.y * fade(180, 0.025)` | blended (SrcAlpha, InvSrcAlpha), colour only, depth tested and written | after the effects |
| `OuterCloud` | `AlphaFadeTexShiftScalePS(1, -0.008, 180, 0.025)`: the albedo at `uv - (0, 0.008 age)`, unlit; alpha `albedo.a * normals(uv).g * fade(180, 0.025)` | blended, colour only, depth tested **strictly less**, not written | after the water, before the effects |
| `NukeUEF` | `NukeHeadPS(1, 0.018, 50, 0.015)`: the normals read as noise at `uv * 6` and `uv * 4` (shifted up `1.25 * 0.018 age`) and `uv * 0.25` (`0.1 * 0.018 age`), `n2`, `n3`, `n4`; a grey `((n3.g + n2.b) / 2 + n4.b) / 2 + n4.r / 2`, lerped halfway to the albedo, plus `0.3 n2.b`, times `n3.g`, plus `n2.b`; alpha `albedo.a * fade(50, 0.015)`, 0 from 20 ticks old where `n2.a * n3.b * n2.g >= albedo.a`, times `n2.g`; tested over `0x0F` | blended, colour only, depth tested and written | after the water, before the effects |

`fade(start, rate)` is `saturate(1 - (age - start) * rate)`. The blended
three clamp their colour before they blend, as FA's 8-bit target does (the
scene is half floats here).

## Rules chosen where mesh.fx is implicit

- **Unset states are D3D9's defaults.** `Effect` names no alpha or depth
  state; `Depth_Enable` (`Cloud`, `NukeUEF`) sets only `ZEnable`. The engine
  reads an unset state as D3D9's default, as it already does for `Unit`
  (which names none either): no blending, depth written, `LESSEQUAL`. So
  `Effect` is opaque and its alpha is the frame's glow. If Moho instead
  inherited the previous draw's state, `Effect` would differ; no capture of
  the original settles it.
- **`Cloud`'s `material.y`.** The technique's `parameter` is
  `PARAM_UNUSED`; the engine feeds every technique but the shields the
  fraction complete (1 for a projectile), so a whole effect's alpha is its
  albedo's.
- **No shadows.** None of the four has `STAGE_DEPTH` in its `renderStage`.
  `Effect` names `depthTechnique = "DepthClip"`, but so does
  `NormalMappedAlphaNoShadow`, which by its name casts none: the stage
  decides.
- **The alpha test** compares the shader's float alpha with `15 / 255`; D3D9
  compares the 8-bit value written.
- **Sorting.** Within a stage, meshes draw by their blueprint's `SortOrder`,
  then the opaque before the blended (M211k); there is no sorting by
  distance from the camera, so overlapping clouds may blend in either order.
- `Rasterizer_Cull_CW` is the engine's back-face culling, as for every mesh
  technique.

## The engine

- `mesh_technique` maps the four names (the legacy ones resolve first).
- `is_blended_technique`: `Cloud`, `OuterCloud`, `NukeUEF`, which therefore
  draw with the fading pipeline (blended, colour only, depth written).
- `OuterCloud` takes a pipeline of its own: the build cube's state (blended,
  colour only, no depth write) with the compare `LESS` (`PipelineBuilder`
  gains `set_depth_compare`).
- Stages: `Effect` and `Cloud` after the effects, `OuterCloud` and `NukeUEF`
  after the water. `has_depth_stage` is false for the four.
- `mesh_frag` computes the four in one branch before the lit techniques.

## Tests

`--effect-mesh-test` stands plates with flat textures under a sun 37 degrees
from overhead (0.5), an ambience of 0.25 and a fill of 0.2, so that `Unit`
lights a plate 0.72, the ambience alone 0.4, and the unlit show their albedo.

- Test 8, `NukeUEF`: the closed form of its colour and alpha; standing over
  an `Effect` plate, its depth hides it; a faint dome dissolves after 20
  ticks and the alpha test uncovers the plate; at 70 ticks old its alpha is
  0.7 of its first, and so it is for a dome made 50 ticks before the clock
  wraps at 36000 ticks, 20 ticks after it.
- Test 9, `OuterCloud`: its colour and masked alpha; over an `Effect` plate
  the plate shows (no depth written); level with a `Unit` plate it is hidden
  (strictly less); at 200 ticks old its alpha is half.
- Test 10, `Cloud`: lit by the ambience alone, its alpha by the fraction
  complete; at about 200 ticks old, half.
- Test 11, `Effect`: opaque and unlit, whatever its albedo's alpha.
- Test 12: a UEF commander's death nuke; its dome resolves to `NukeUEF` and
  is drawn. `OSC_EFFECT_MESH_TEST_PNG=<prefix>` writes two frames of it.
- `test_mesh_technique.cpp`: the names, stages, blending and shadows.

What they do not prove:

- **Texture motion and sampling scales.** The textures are flat, so the
  scrolls (`Effect`'s, `OuterCloud`'s, the dome's noise) and `Effect`'s and
  `NukeUEF`'s tiling could be wrong and pass.
- **`Effect`'s glow** and the blended techniques' unwritten alpha: the tests
  read colour only, and offscreen frames have no bloom.
- **The exact `0x0F` cutoff**, the culling (the plates are wound both ways),
  and that the real dome looks as the original's: no capture of the
  original's nuke exists.

## Left

`Explosion`, `NukeEMP`, `NukeQuantum`, `GlassAlpha`, `PhalanxEffect`,
`UEFQuantumGate` and `Flat` still draw as `Unit`; a check that lists the
ShaderNames with no technique; a comparison with the original's nuke.
