# M211q: the zapper's sphere and the quantum gateway's field

Two script entities that stand on common structures drew as opaque, lit
meshes. Their mesh blueprints name `ShaderName = 'PhalanxEffect'` and
`'UEFQuantumGate'`, which `mesh_technique` didn't know, so both fell back to
`Unit`. Retail `mesh.fx` is read for the formulas and states, never copied.

- `PhalanxEffect`: `CybranPhalanxSphere01_mesh` and `…02_mesh` (the
  `Sphere01` mesh), which `CAMZapperWeapon` (`cybranweapons.lua`) sets on an
  entity at its muzzle, idle and with a target. The Cybran tactical missile
  defence (URB4201) and carrier (URS0303) carry it; the cruiser's and the
  battleship's `CAMZapperWeapon02` don't. A solid ball sat on every Cybran
  TMD.
- `UEFQuantumGate`: `ForceField01_mesh`, which the UEF quantum gateway
  (UEB0304) sets on an entity between its arms once built. An opaque, lit
  plate stood in the gate.

FAF's data and `mesh.fx` have the same techniques and blueprints (FAF adds a
third sphere, `CybranPhalanxSphere03_mesh`).

## The techniques

Both read the instance's age as `EffectVS` does (M211o) and set no
`STAGE_DEPTH` or `STAGE_REFLECTION`: no shadow, no reflection, no fidelity
lanes. Their parameter is `PARAM_UNUSED`.

| Technique | Shaders | Render state | Stage |
|---|---|---|---|
| `PhalanxEffect` | `EffectVertexNormalLoFiVS(1, 1, -0.05, 0, -0.05, -0.025)`: the albedo at `uv - (0.05 age, 0)`, the normals at `uv - (0.05, 0.025) age`; `TwoTexShiftScalePS(0.2, 0.2)`: `(albedo.rgb + normals.rgb) * 0.2 * albedo.a`, alpha 0.2 | added: `One`, `InvSrcAlpha`, RGBA, so what is behind keeps 0.8 of its colour and glow; depth tested strictly less, not written; culled | after the water, before the effects |
| `UEFQuantumGate` | `ThreeUVTexShiftScaleLoFiVS(1, 4, 1, 0, 0.025, 0.025, 0.025, 0, 0)`: the albedo at `uv + (0, 0.025 age)`, the normals at `4 uv + 0.025 age`, the albedo again at `uv`; `TexSubAlphaMaskStaticLoFiPS(1, 0.4)`: the first less the second, alpha 0.4 times the third's alpha | blended (SrcAlpha, InvSrcAlpha), RGBA; depth tested strictly less, not written; **not culled** | after the water, before the effects |

## Rules chosen where mesh.fx is implicit

- **The normals as a colour.** Both read the LOD's `NormalsName` as a plain
  texture, as `NukeUEF` does (M211o).
- **Negative colour.** `UEFQuantumGate`'s difference can go below 0; FA's
  8-bit target clamps what the shader writes before it blends (M211n), and so
  does the engine's.
- **Alpha blends by the colour's factors** (no `SeparateAlphaBlendEnable`):
  `PhalanxEffect` leaves a glow of `0.2 + 0.8 glow`, `UEFQuantumGate` of
  `a² + glow (1 - a)`.

## The engine

- `mesh_technique` maps the two names; both are blended, after the water,
  with no depth stage.
- Two pipelines: `phalanx` (`One`, `InvSrcAlpha`, RGBA, strictly less, no
  depth written) and `quantum_gate` (`nuke_emp`'s state, unculled).
  `build_mesh` sets the colour's blend factors as well as alpha's.
- `mesh_frag` computes both among the effects' techniques, from the texture
  coordinate and the age (the vertex shaders' shifts are linear, so the same
  at each pixel).

## Tests

`--effect-mesh-test`, under Tests 8-11's sun:

- Test 18, `PhalanxEffect`: over a `Unit` plate, the plate's colour times 0.8
  plus the sphere's; over an `Effect` plate the plate shows (no depth
  written); a striped plate's stripes move 0.1 of it in 2 ticks.
- Test 19, `UEFQuantumGate`: its colour and alpha; plates wound one way and
  the other: one `Unit` plate shows and one doesn't, both gate plates show;
  a banded plate turned so that v runs across the frame moves 0.075 of it in
  3 ticks.
- Test 20: a Cybran TMD (URB4201) and a UEF quantum gateway (UEB0304), made
  by `CreateUnitHPR`, carry meshes drawn by the two techniques.
- `test_mesh_technique.cpp`, `--dispatch-matrix-test`: names, stages,
  blending, shadows, pipelines.
- The plates' blueprints gain ten structures, so the test has a plate for
  each check.

What they do not prove: the normals' own scroll (the plates' normals are
flat) and that the sphere and the field look as the original's in play.

## Left

`Explosion`, `GlassAlpha`, `Flat` and the rest of
`tools/shader_names_baseline.txt`.
