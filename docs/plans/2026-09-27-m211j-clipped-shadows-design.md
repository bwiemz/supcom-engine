# M211j: shadows cut by their albedo's alpha

The shadow pass drew every mesh whole: a tree's shadow was the shadow of
the cards its leaves are painted on, a fence's the shadow of its panel. FA
draws a mesh into the shadow map with its technique's `depthTechnique`,
and the alpha-tested techniques name `DepthClip`: `DepthVS` and
`DepthPS(clipTest = true)`, which discards where the albedo's alpha is
under a half (`clip(albedo.a - 0.5)`). The swaying trees name
`UndulatingDepthClip`, which sways the vertex as `UndulatingNormalMappedVS`
does, then clips the same. Retail `mesh.fx` is read for the formulas, never
copied.

| Technique | depthTechnique |
|---|---|
| `NormalMappedAlpha`, `BlackenedNormalMappedAlpha`, `VertexNormal` | `DepthClip` |
| `UndulatingNormalMappedAlpha` | `UndulatingDepthClip` |
| props whose technique isn't ported | `DepthClip` (the engine draws them as `NormalMappedAlpha`) |
| the rest | `Depth`, unclipped, or none |

The clip reads the albedo's alpha alone: no fraction complete, unlike the
colour pass's test.

## The engine

- The mesh shadow pipeline gets a fragment shader of its own
  (`shadow_mesh_frag`) and the albedo at set 1; the pass binds each group's.
- `shadow_mesh_vert` passes the UV and whether the instance is a prop (its
  colour's negative green), and sways `UndulatingNormalMappedAlpha` by FA's
  time (the push block gains it).
- The fragment shader discards where a clipped technique's albedo alpha is
  under a half.

## Tests

`--clipped-shadow-test` (gate): small plates hover 2.5 over a unit's plate,
the sun to one side, their albedo opaque on one half and clear on the other:

1. Drawn by `Unit`, the whole plate casts its shadow; by `NormalMappedAlpha`,
   `VertexNormal`, `UndulatingNormalMappedAlpha`, and as a prop of an
   unported technique, half of it.
2. An undulating wall's shadow moves over half a sway; a
   `NormalMappedAlpha` wall's doesn't.

## Left for later

- `Flat` and `Effect` also name `DepthClip`; the engine hasn't ported them.
