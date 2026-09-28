# M212b: The map's decals, projected and lit

## Why

A map's decals are its roads, rocks, craters, dirt and scorch. SCMP_009
alone has hundreds. The engine drew them as flat quads, which float over or
sink into the ground on any slope. It also left them unlit, showing only
their albedo, so they stood out against the lit terrain. And every type but
normals went down the albedo path, so glow masks, water masks and
alpha-normal decals drew their masks and normals as colours.

FA projects a decal onto the terrain under it and lights it by the terrain's
formula. The rules come from:

- terrain.fx's `DecalsVS`, `DecalsPS`, `DecalAlbedoXP`, and the `TDecals` /
  `TDecalsXP` techniques;
- faf-re's `CWldTerrainDecal` (types, `Update`, the LOD fade);
- faf-re's `HighFidelityTerrain` (`UpdateRenderContext`, `DrawDecalPass`)
  and `TerrainCommon`, for the mask;
- faf-re's `GeomCamera3` (the LOD metric) and `RuntimeTuningGlobals` (the
  defaults).

Retail's files are read for the rules, never copied.

## The rules

### Types

`CWldTerrainDecal::sTypeDesc`:

| Type | Name |
| --- | --- |
| 1 | Albedo |
| 2 | Normals |
| 3 | Water Mask |
| 4 | Water Albedo |
| 5 | Water Normals |
| 6 | Glow |
| 7 | Alpha Normals |
| 8 | Glow Mask |
| 9 | AlbedoXP |

Albedo decals draw with `TDecals`, AlbedoXP with `TDecalsXP`. The type
chooses the technique, not the map's terrain shader. Normals and Alpha
Normals draw into the terrain's normals (`TDecalsNormals`, `…Alpha`). The
others have techniques of their own (glow, water).

### Geometry (`UpdateRenderContext`)

- **The footprint.** A decal is placed by its corner. Its footprint runs
  from its position along its x and z axes, scaled: `(sx·cos ry, sx·sin ry)`
  and `(−sz·sin ry, sz·cos ry)`. The bounds are the footprint's extent
  (`ProjectDecalBoundsXZ`).
- **The triangles.** A decal draws the terrain's own triangles in its bounds
  (`CollectClippedCollisionIndicesInRect`). A decal lying flat, within
  `ren_DecalFlatTol` (0.01), draws one quad instead, which gives the same
  image.
- **The projection.** `DecalsVS` projects each vertex by the decal's texture
  matrix:
  - the matrix is `T(−position) · RotY(ry) · RotX(rx) · RotZ(rz)` (D3DX
    rotations), then scaled by `1/scale`;
  - the decal's UV is `(world · matrix).xz`, which is 0 to 1 over the
    footprint;
  - its samplers clamp.
- **Depth.** Depth is tested LessEqual and not written, with a depth bias of
  `decalDepthOffset` (−0.00001). `decalHeightOffset` is 0.

### The pixel

- **`DecalsPS` (Albedo).** It is terrain.fx's `CalculateLighting` with the
  decal's albedo:
  - the specular amount is the specular texture's red;
  - the normal is the terrain's (its normal buffer: strata and normal
    decals);
  - it takes the shadow, and the water tint by depth (`ApplyWaterColor`);
  - alpha is `albedo.a · mask.r · DecalAlpha`.
- **`DecalAlbedoXP` (AlbedoXP).** The XP terrain's light, with specular
  `pow(saturate(reflect(V, N)·S), 80) · spec.a · SpecularColor.a ·
  SpecularColor.rgb`, and the same water tint. Alpha is
  `albedo.a · mask.a · DecalAlpha`.
- **The blend.** SrcAlpha / InvSrcAlpha, RGB only.
- **The textures.** The decal's first texture is its albedo and its second
  its specular. The mask is retail's `/textures/engine/decalMask.dds`, one
  for every decal (`TerrainCommon`).

### The fade (`GetLODAlpha`)

- **The metric.** `worldDistance` is the camera's `viewport.r[1]` dotted with
  the decal's middle (at the terrain's height there): the width the screen
  spans, in world units, at the middle's view depth. For a symmetric
  frustum that is `2·tan(hfov/2)·depth`, times `lodScale` (1).
- **The fade.**
  - `fadeBegin = cutoff · ren_DecalFadeFraction` (0.75), and
    `alpha = 1 − (clamp(d, fadeBegin, cutoff) − fadeBegin) / (cutoff − fadeBegin)`.
  - A near cutoff fades the other way.
  - Below 1/255 the decal isn't drawn.

## The engine

- **Map data.**
  - `map::DecalInfo` keeps the decal's type, its second texture and its near
    cutoff.
  - The scenario loader routes decals by type:
    - Albedo and AlbedoXP go to the lit decals;
    - Normals and Alpha Normals go to the normal overlay (Alpha Normals were
      drawn as albedo);
    - Water Mask, Water Albedo, Water Normals, Glow and Glow Mask are kept
      but not drawn until M212c.
  - The normal overlay's bake places a decal by its corner too. It had
    centred them, as the old quads did, so both were half a decal off FA's
    placement.
- **Geometry.**
  - `TerrainMesh` collects the indices of its quads in a rectangle.
  - At map load the renderer builds one decal index buffer over the
    terrain's vertex buffer, with each decal's range and texture matrix.
- **Shaders.**
  - The terrain's surface rules (its bindings, the normal blend,
    `CalculateLighting`, the water tint) become one GLSL block shared by
    `terrain_frag` and the new `decal_lit_frag`.
  - `decal_lit_vert` takes the terrain's vertices and outputs the decal's
    UV per vertex, with the depth bias.
- **Pipeline.**
  - It binds the terrain's descriptor set and its light set, then a set for
    the decal's albedo, specular and the mask.
  - The push block holds, within Vulkan's guaranteed 128 bytes: the
    view-projection, the matrix's two UV rows, the map size, the eye, the
    alpha and the XP flag.
- **Per frame.** The metric, alpha and draw for each decal. The old quads,
  their pipeline and their buffers go.

## Tests

`--decal-render-test` (gate). It uses flat ground of the test's own (and a
slope, and ground under water) with decals of its own. It compares frames
at the same view: with decals and without, or under the map's light and a
white fill, which is light 1, the albedo alone. The offscreen shots draw no
decals by default, and the test turns them on.

1. **Placed by its corner.** A red decal 8 by 4, turned a quarter, colours
   x 20–24 and z 24–32 from its corner at (24, 24). It colours none of the
   ground a centred decal would cover.
2. **Lit.** A grey decal's light, its frame under SCMP_009's light over its
   frame under the white fill, is FA's flat-ground formula, to within 0.003.
3. **Masked.**
   - At albedo alpha a half, a decal is half over the ground.
   - Turned an eighth, a decal's bounding rectangle takes nothing outside
     its diamond: the mask's border, clamped.
4. **The specular's channel.** Under a sun that the view glints off:
   - an Albedo decal glints by its specular's red and not its alpha;
   - an AlbedoXP decal glints by the alpha and not the red.
5. **The fade.** At the camera's metric for its middle, a decal is:
   - whole within ¾ of its cutoff;
   - half at ⅞;
   - gone past the cutoff.
6. **Projected.** On a slope rising 1 in 4, a decal colours the surface at
   both its ends.
7. **Types.** A Glow Mask decal draws nothing (until M212c).
8. **Under the water.** A red decal on ground 10 under the water looks as the
   tinted ground beside it does.

Map decals' textures load with the map (blocking), as Moho loads them, so
the first frame shows them.

## Left for later (M212c)

- The Water Albedo, Water Mask, Glow and Glow Mask techniques.
- Runtime splats (`CreateSplat`, scorch marks) and their compositing.
- Decal fidelity (`ren_DecalFidelity`'s area threshold).
