# M211s: the CZAR's technique

The CZAR (UAA0310) and the UEF Novax defence satellite (XEA0002) name
`ShaderName = 'AeonCZAR'` for their first LOD (to 500), `Aeon` for the
second. `mesh_technique` didn't know `AeonCZAR`, so up close both drew as
`Unit`: `NormalMappedPS`, the sun at full strength and UEF's highlight, with
no Aeon look and none of the CZAR's moving energy. Retail `mesh.fx` is read
for the formulas and states, never copied; FAF's has the same technique.

## The technique

`AeonCZAR` has three lanes, all opaque (no alpha state), `NormalMappedVS`,
with a depth stage (`Depth`), a reflection, before the water:

- **High:** `AeonCZARPS(true)`. `AeonPS`'s light (the sun at 0.6, the
  environment by the specular's red, the glow by its blue), but its own
  highlight, `pow(phong, 8) * specular.g` without `AeonPhongCoeff`, and the
  `<default>` cube: unlike `Aeon_HighFidelity`, it names no `environment`.
  Then the LOD's `SecondaryName`, read twice, at 60 times the texture
  coordinate shifted `-(0.16, 0.01)` a tick and at 30 times shifted
  `(0.08, -0.005)`: their blue and green, where the albedo's alpha is low,
  add `(0.2, 0.7, 1) * (b + g) * (1 - albedo.a)` to the colour and the same
  `(b + g) * (1 - albedo.a)` to the glow.
- **Medium:** `AeonPS(false)`, `Aeon_MedFidelity`'s.
- **Low:** `ColorMaskPS_LowFidelity`, `Aeon_LowFidelity`'s.

The satellite names no `SecondaryName`; the engine binds transparent black
there, so its energy is 0.

## The engine

- `mesh_technique` maps `AeonCZAR`. Below High the renderer draws it as
  `Aeon` (the lanes are the same), as it already draws a Seraphim personal
  shield's Low lane.
- `mesh_frag`'s lit branch computes `AeonCZARPS` at High.

## Tests

`--material-test`, in the dark (no sun, ambience or fill):

- Test 22: an `AeonCZAR` plate whose albedo's alpha is clear shows
  `(0.2, 0.7, 1)` times its secondary's blue and green; an `Aeon` plate, and
  an `AeonCZAR` plate with an opaque albedo, show black. A mirror plate
  (specular red 1) shows the `<default>` cube and not `<aeon>`. At Medium the
  first plate is black (`AeonPS`).
- Test 23: the CZAR's first LOD resolves to `AeonCZAR` and is drawn.
- `test_mesh_technique.cpp`; `--dispatch-matrix-test` checks the lane each
  fidelity draws (`Aeon` at Low and Medium, `AeonCZAR` at High).

What they do not prove: the energy's scroll and tiling (the secondary is
flat), and the highlight's exponent.

## Left

`Explosion`, `GlassAlpha`, `Flat` and the rest of
`tools/shader_names_baseline.txt`.

*Done (2026-10-25) — M211t: `Flat`.*
