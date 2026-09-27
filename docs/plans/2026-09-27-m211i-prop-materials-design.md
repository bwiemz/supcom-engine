# M211i: the props' materials

Props drew with one rule of the engine's: `NormalMappedPS`, untinted,
alpha-tested over `0x80`. FA's props name their own techniques, and three of
them cover 313 of retail's blueprints: `VertexNormal` (with its legacy
`TMeshNoNormals`: rocks, pipes, vents), `NormalMappedTerrain` (clutter that
sits in the ground) and `UndulatingNormalMappedAlpha` (the Evergreen birches
and other trees, swaying). Retail `mesh.fx` is read for the formulas, never
copied.

## The techniques

| Technique | FA | Render state |
|---|---|---|
| `VertexNormal` | `VertexNormalVS` + `VertexNormalPS_HighFidelity`: the albedo times the instance's colour, lit by the **vertex's** normal (no normal map), shadowed; alpha `f * albedo.a`, tested over `0x23` | **blended** by that alpha, colour only |
| `NormalMappedTerrain` | `NormalMappedVS` + `NormalMappedTerrainPS`: the albedo times the instance's colour, lit by the normal map but **unshadowed**; no highlight, no reflection; alpha `glowMinimum` | opaque, colour and alpha; depth less |
| `UndulatingNormalMappedAlpha` | `UndulatingNormalMappedVS` + `NormalMappedPS` as `NormalMappedAlpha`: each vertex moves `0.003 y * sin²(0.05 time - wind . p) * wind` in the world, `y` its height in the mesh, `p` the instance's position, `wind` FA's `windDirection` | as `NormalMappedAlpha` |

`windDirection` is `(0.707, 0, 0.707)`: `mesh.fx`'s own default. Moho never
sets it (faf-re's `MeshShaderVarSet` registers no such variable). A tree ten
units tall sways three centimetres at its top, once every 63 ticks.

## The engine

- Three techniques in `MeshTechnique` (15-17); `TMeshNoNormals` already
  resolves to `VertexNormal` (M211g).
- `VertexNormal` draws with the blended groups (the fade pipeline: colour
  only).
- The mesh vertex shader sways `UndulatingNormalMappedAlpha`'s vertices; its
  pixel shader is `NormalMappedAlpha`'s.
- The props' generic rule stays for props whose technique isn't ported.

## Tests

`--prop-material-test` (gate), plates of the test's own standing in for
props, over the sky's clear colour:

1. **The retail props.** Rocks (`VertexNormal`), Crystalline clutter
   (`NormalMappedTerrain`) and Evergreen birches (`UndulatingNormalMapped
   Alpha`) resolve to their techniques.
2. **`VertexNormal`.** Half-alpha albedo blended at 0.5; under `0x23`, cut.
   A normal map tilted away from the sun: `NormalMappedAlpha` dims, and
   `VertexNormal`, lit by the vertex's normal, doesn't.
3. **`NormalMappedTerrain`.** Under a hovering plate, unshadowed; with a
   SpecTeam that highlights, no highlight.
4. **`UndulatingNormalMappedAlpha`.** A tall wall of it sways between frames
   ticks apart; one of `NormalMappedAlpha` doesn't.

## Left for later

- `DepthClip` and `UndulatingDepthClip`: FA cuts these props' shadows by their
  alpha (and sways them).
- `Clutter`, `UndulatingClutter`, `UnderwaterClutter`,
  `BloatingNormalMappedAlpha`, `NormalMappedAlphaNoShadow`.
