# M211v: the swelling pods and the objective arrow

Two variants of `NormalMappedAlpha` drew as `Unit`, which `mesh_technique`
falls back to for a name it doesn't know. Retail `mesh.fx` is read for the
formulas and states, never copied; FAF's has the same techniques.

- `BloatingNormalMappedAlpha`: the RedRocks pods (`Pod01_prop`,
  `Pod02_prop`), 1,813 of them on three retail maps (219 on Theta Passage,
  936 on The Bermuda Locket, 658 on Syrtis Major). They stood still.
- `NormalMappedAlphaNoShadow`: `/meshes/game/arrow_mesh`, the arrow
  `objectiveArrow.lua` hangs over a campaign objective. It cast a shadow
  and drew with the team's colour masked in.

## The techniques

Both are `NormalMappedAlpha`'s lanes in their pixel shaders (`NormalMappedPS`
unmasked, alpha-tested over `0x80` by `f * albedo.a`; Low,
`VertexNormalPS_LowFidelity`), and both draw after the water.

- **Bloating** moves each vertex along its direction from the mesh's origin
  by `2 * |dir.xz| * sin(0.1 time + |p.xz|)` (`p` the instance's place), at
  every lane (`BloatingNormalMappedVS`, `BloatingVertexNormalVS`). It has a
  depth stage, but its `depthTechnique` is `UndulatingDepthClip`: its shadow
  sways in the wind as a tree's does instead of swelling. That is retail's
  data, kept.
- **NoShadow** has no `STAGE_DEPTH`: it casts no shadow. Its Low lane drops
  the reflection (the engine reflects every unit, M211r's Left).

## The engine

- `mesh_technique` maps both; both draw after the water.
- Bloating: `mesh_vert` swells it, `mesh_frag` draws it as the undulating
  trees (unmasked, alpha-tested), the shadow pass sways and clips it as
  theirs.
- NoShadow draws as `NormalMappedAlpha`, with no depth stage.

## Tests

- `--prop-material-test` Test 5: a Bloating plate, 8 across, is 2.09 times as
  wide at the swell's top (10.83) as at its bottom (5.17); a
  `NormalMappedAlpha` plate doesn't change.
- `--clipped-shadow-test` Test 2: a Bloating wall's shadow sways as the
  undulating wall's; Test 3: a hovering `NormalMappedAlphaNoShadow` plate
  casts no shadow where a `NormalMappedAlpha` one does.
- `test_mesh_technique.cpp`; `--dispatch-matrix-test` (NoShadow drawn as
  `NormalMappedAlpha`, after the water).

## Left

- `Shield` (`TMeshShield`), `EditorMarker` and `Explosion`: the games don't show them. No retail
  map places `sphere_prop`, the one prop naming `TMeshShield`; only the editor makes marker
  props; the call that would make `Explosion` meshes is commented out in retail's and FAF's Lua.
- `Clutter`, `UnderwaterClutter`, `UndulatingClutter`: they need Moho's clutter system, which no
  retail terrain type turns on.
