# M211u: the glass domes' technique, GlassAlpha

Six mesh blueprints name `ShaderName = 'GlassAlpha'`, the glass that unit
scripts set on entities over their structures: the domes of the Aeon
torpedo launchers (UAB2109, UAB2205) and the domes and windows of the
civilian buildings (UAC1401, XAC1401, UAC1901, UEC1401, URC1401).
`mesh_technique` didn't know it, so they drew as `Unit`: opaque, hiding what
stands under them, and casting a shadow. Retail `mesh.fx` is read for the
formulas and states, never copied; FAF's has the same technique.

## The technique

`GlassAlpha` has a Medium and a Low lane and no High one; Moho fills a
missing lane from the next (`FinalizeMissingImplementations`), so at High
it draws Medium's. Both are blended (`SrcAlpha`, `InvSrcAlpha`), colour
only, depth tested `LessEqual` and not written, culled, after the water and
the effects, with no depth stage (no shadow) and no reflection.

- **Medium:** `NormalMappedVS`, `GlassAlphaPS(false)`. The albedo with the
  team's colour under the specular's alpha, lit by the sun twice over
  (`2 * sun * N.L * shadow + ambience`, by the multiplier and the fill), its
  glow by the specular's blue; then, where the specular's alpha doesn't
  mask it, the highlight (specular green) and half the `<default>` cube by
  the specular's red (it names no `environment`). Alpha: the albedo's, plus
  the specular's blue, plus its green where unmasked. One shadow tap.
- **Low:** `VertexNormalVS`, `GlassAlphaLoFiPS`: `2 * light² * albedo`, the
  team's colour as above; alpha the albedo's plus the specular's blue.

## The engine

- `mesh_technique` maps `GlassAlpha`: blended, after the effects, no depth
  stage. It draws with the build cube's pipeline (blended colour, tested,
  unwritten), its High lane as Medium (with Medium's one shadow tap).
- `mesh_frag` computes both lanes, clamped before they blend.

## Tests

- `--effect-mesh-test` Test 25, under Tests 8-11's sun: a plate lit twice
  over and blended by its albedo's alpha; a Cloud plate drawn after it and
  below it shows through it (no depth written); the lane at High is
  Medium's, at Low Low's; an Aeon torpedo launcher's dome draws by
  `GlassAlpha`.
- `test_mesh_technique.cpp`; `--dispatch-matrix-test`.

What they do not prove: the highlight, the environment and the team's
colour (the plates' specular is black), and the Low lane's colour.

## Left

`Explosion` and the rest of `tools/shader_names_baseline.txt`.
