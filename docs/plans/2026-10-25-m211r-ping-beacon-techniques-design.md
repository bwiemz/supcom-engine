# M211r: the pings' and the ferry beacons' techniques

A ping drew as an opaque, lit model hovering over the ground, hidden behind
hills and units (once shown at all: until the entity's spec gave it its
army, the commit before, it hid itself as a neutral's), and a ferry route's
beacon as an opaque, lit flag casting a shadow. Their meshes name `ShaderName = 'CommandFeedback3'` (the pings'
`/meshes/game/ping_alert_marker`, `ping_attack_marker`, `ping_move`) and
`'CommandFeedback4'` (the beacons UAB5102, UEB5102, URB5102 and XSB5102, all
`/meshes/game/Ferry_lod0.scm`), which `mesh_technique` didn't know, so both
fell back to `Unit`. Retail `mesh.fx` is read for the formulas and states,
never copied.

A ping is retail's `SimPing.SpawnPing`: an entity two above the clicked
point, seen by the pinging army and its allies for its lifetime, its mesh
`/meshes/game/ping_<kind>`, its scale pulsing from `AnimatePingMesh`. A
beacon is the unit the engine places on a ferry route (`sweep_ferry_beacons`
keeps it while a route needs it). FAF's data and `mesh.fx` have the same
techniques, and FAF adds `ping_nuke_marker`.

## The techniques

Both take `VertexNormalVS` and `CommandFeedbackPS0(false)`: the albedo,
unlit, at its own alpha; no fade over a lifetime (their parameter is
`PARAM_UNUSED`) and no alpha test. Unlike `CommandFeedback` and `RallyPoint`
(`CommandFeedbackVS`), the mesh keeps its size. Blended (SrcAlpha,
InvSrcAlpha), colour only, after the water and before the effects; no
shadow, no reflection, no fidelity lanes.

| Technique | Depth |
|---|---|
| `CommandFeedback3` | `Depth_Disable`: drawn over everything, as the order marks are |
| `CommandFeedback4` | `Depth_Enable_LessEqual_Write_None` and `DepthBias = -0.0001`: tested, not written, pulled a ten-thousandth of the depth range nearer |

## Rules chosen where mesh.fx is implicit

- **The bias** is D3D9's, added to the depth after the divide; the engine's
  depth is D3D's `[0, 1]` with the same compare, so the vertex shader
  subtracts `0.0001 w` from the clip position's z. At the tests' 30 units
  that is about 0.3 units of the world.
- **Reflection.** The engine reflects every unit (M213b), so a beacon over
  the water is reflected; `CommandFeedback4` names no `STAGE_REFLECTION`.
  Left as it is.

## The engine

- `mesh_technique` maps the two names: blended, after the water, no depth
  stage. They are not `is_feedback_technique`, which also scales the
  `CommandFeedbackVS` marks with the distance.
- `CommandFeedback3` draws with the order marks' pipeline (no depth test),
  `CommandFeedback4` with the build cube's (tested, unwritten).
- `mesh_frag` writes the albedo as it is; `mesh_vert` biases
  `CommandFeedback4`'s depth.

## Tests

`--effect-mesh-test`:

- Test 21, `CommandFeedback3`: under a `Unit` plate standing 2 above it, it
  shows over that plate, the same 50 ticks on; at alpha 30 (under the order
  marks' 0x23 test) it still shows.
- Test 22, `CommandFeedback4`: 2 under a `Unit` plate it is hidden; 0.1
  under one, within its bias, it shows.
- Test 23: retail's `SpawnPing` and a UEF ferry beacon (UEB5102) draw by the
  two techniques.
- `test_mesh_technique.cpp`: a case for their stage, blending and shadow;
  `--dispatch-matrix-test` covers them.

What they do not prove: the bias's exact size (only that it lies between
0.1 and 2 units at 30).

## Left

A technique's `STAGE_REFLECTION` (the engine reflects every unit);
`Explosion`, `GlassAlpha`, `Flat` and the rest of
`tools/shader_names_baseline.txt`.

*Done (2026-10-25) — M211s: `AeonCZAR`.*
