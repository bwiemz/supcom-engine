# M211p: the Aeon and Cybran nukes' effect techniques

An Aeon or Cybran nuke drew an opaque, lit sphere where it went off. The
Cybran nuke's sphere (`EMPFluxWarheadEffect03`, the `Sphere01` mesh at 5.4)
names `ShaderName = 'NukeEMP'`, the Aeon nuke's head
(`QuantumWarheadEffect01`) `TMeshQuantumNuke`, which resolves to
`NukeQuantum`; `mesh_technique` knew neither, so both fell back to `Unit`.
Retail `mesh.fx` is read for the formulas and states, never copied.

In retail's data `CIFEMPFluxWarhead02` makes the sphere beside the two
`OuterCloud` clouds (M211o), and `AIFQuantumWarhead02`'s `DistortionField`
makes the head. Those are the warheads of the Cybran and Aeon strategic
launchers (URB2305, UAB2305), their missile submarines (URS0304, UAS0304),
the Paragon's death (XAB1401), and the Cybran and Aeon commanders'
`DeathWeapon`s (URL0001, UAL0001): every game that ends with one of those
commanders' deaths shows one. FAF's data has the same techniques and
blueprints, and also draws its Cybran teleport (`CybranTeleport_mesh`) with
`NukeEMP`.

## The techniques

Both take `EffectVS` (the instance's age in ticks, as M211o's) and
`AlphaFadeTexShiftScalePS(texScale, timeCoeff, timeFade, fadeMultiplier)`,
the shader `OuterCloud` draws with: the albedo at `texScale * uv`, shifted
`timeCoeff * age` along v; its alpha times the normals' green at `uv` and
`fade(timeFade, fadeMultiplier)`, `saturate(1 - (age - start) * rate)`.

| Technique | Pixel shader | Render state | Stage |
|---|---|---|---|
| `NukeEMP` | `(1, -0.008, 180, 0.025)`, as `OuterCloud` | blended (SrcAlpha, InvSrcAlpha), **RGBA**; depth tested strictly less, not written | after the water, before the effects |
| `NukeQuantum` | `(0.5, -0.006, 150, 0.025)` | blended, **red, blue and alpha** (`ColorWriteEnable = 0x0D`); depth tested strictly less, not written | after the effects |

Neither has `STAGE_DEPTH` (no shadow) or `STAGE_REFLECTION`, nor fidelity
lanes.

## Rules chosen where mesh.fx is implicit

- **The glow.** A technique that writes alpha writes the frame's glow
  (M211e). D3D9 blends alpha by the colour's factors unless
  `SeparateAlphaBlendEnable` is set, which mesh.fx never sets, so the glow
  becomes `a² + glow · (1 - a)`, as the shields' (M211k).
- **The green.** `NukeQuantum`'s state, `AlphaBlend_SrcAlpha_InvSrcAlpha_Write_RBA`
  in `d3d9states.compat`, leaves green out (FAF's is the same); the engine
  draws it as written, whatever the intent.
- **Sorting.** As M211o: by SortOrder, then the opaque before the blended.

## The engine

- `mesh_technique` maps the two names (`TMeshEMPNuke` and `TMeshQuantumNuke`
  resolve first).
- `is_blended_technique`: both; `is_post_water_technique`: `NukeEMP`;
  `is_post_effect_technique`: `NukeQuantum`; `has_depth_stage`: neither.
- Two pipelines, OuterCloud's state with alpha written (`nuke_emp`) and with
  red, blue and alpha written (`nuke_quantum`).
- `mesh_frag`'s `AlphaFadeTexShiftScalePS` branch takes the three
  techniques' parameters.
- The frame's draw counts are sized by the technique enum, not a literal
  that stopped at `NukeUEF`.

## Tests

`--effect-mesh-test`, under Tests 8-11's sun:

- Test 13, `NukeEMP`: its colour and masked alpha; over an `Effect` plate
  the plate shows (no depth written); level with a `Unit` plate it is hidden
  (strictly less); drawn after the water by its own pipeline.
- Test 14, `NukeQuantum`: over a `Unit` plate red and blue blend and green is
  the plate's; at 170 ticks old its alpha is half; drawn after the effects.
- Test 15, the scroll and the scale: plates banded four times along v, turned
  so that v runs across the frame. Over 10 ticks `NukeEMP`'s bands move 0.08
  of the plate and are a quarter of it wide; `NukeQuantum`'s move 0.12 and
  are half of it wide. `OuterCloud` shares `NukeEMP`'s branch, so this pins
  its scroll too.
- Test 16, the glow: with the bloom on, the sky about a `NukeEMP` or
  `NukeQuantum` plate brightens; about an `OuterCloud` plate, which writes
  colour only, it doesn't.
- Test 17: an Aeon and a Cybran commander die; their nukes' meshes resolve
  to `NukeQuantum` and `NukeEMP` and are drawn. `OSC_EFFECT_MESH_TEST_PNG`
  writes two frames of each.
- `test_mesh_technique.cpp`: the names, stages, blending and shadows;
  `--dispatch-matrix-test` covers both pipelines.

What they do not prove: the exact cutoffs of the fades (a frame at one age
each), and that the original's spheres look so in motion; the frames of the
commanders' deaths are compared with captures of the original by eye.

## Left

`Explosion`, `GlassAlpha`, `PhalanxEffect`, `UEFQuantumGate` and `Flat`, and
the rest of `tools/shader_names_baseline.txt`.
