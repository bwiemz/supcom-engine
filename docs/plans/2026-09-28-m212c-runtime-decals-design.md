# M212c: Runtime decals and splats

## Why

Scripts put marks on the ground as the game runs:

- every structure lays a tarmac (`CreateDecal`: Albedo, Alpha Normals, Glow);
- explosions leave scorch decals and splats (`CreateDecal`, `CreateSplat`);
- beams burn the ground (`CreateSplat`);
- tanks leave tread marks (`CreateSplatOnBone`).

The engine keeps each as an effect and draws none of them. It also ignores
their lifetimes in ticks, their fade when they go, and who may see them.

The rules come from:

- faf-re's `EffectLuaStartupRegistrations` (`cfunc_CreateDecalL`,
  `cfunc_CreateSplatL`, `cfunc_CreateSplatOnBoneL`,
  `CreateDecalFromTransform`);
- `CDecalBuffer` (the sim's handles, their lifetimes, and each army's
  sight of them);
- `CDecalManager` and `CWldSplat` (the user side: adds, removals and
  fades, splat quads);
- `HighFidelityTerrain` (the splat pass and its alpha);
- `CWldSession::DoBeat`, which feeds the manager once a beat;
- terrain.fx's `SplatsVS`, `SplatsPS` and `TSplats`, and
  d3d9states' `Rasterizer_Cull_None_Bias_Neg001`.

Retail's files are read for the rules, never copied.

## The rules

### Making one

- **`CreateDecal(position, heading, tex1, tex2, type, sizeX, sizeZ, lod,
  duration, army[, fidelity])`** returns a handle. Fidelity defaults to 1.
- **`CreateSplat(position, heading, texture, sizeX, sizeZ, lod, duration,
  army[, fidelity])`** returns nothing.
- **`CreateSplatOnBone(entity, offset, bone, texture, sizeX, sizeZ, lod,
  duration, army)`** returns nothing. Its transform is the bone's in the
  world, and the offset is turned by the bone before it is added.
- **The army** is an index (1-based) or a name.
- **The transform.** A heading h is the quaternion (w = cos h/2,
  y = sin h/2). From a transform:
  - `right` and `forward` are its x and z axes, flattened onto the ground
    and normalised;
  - the position is the transform's less half of each side along them:
    `pos − right·sx/2 − forward·sz/2`, which puts the footprint's middle
    at the transform, where a map decal is placed by its corner;
  - the turn is `rotation.y = −atan2(2(xz + wy), 1 − 2(z² + y²))`, the
    angle of `forward.x` over `right.x`, so −h for a heading;
  - the size is (sx, 1, sz).
- **The lifetime.** The removal tick is `tick + floor(duration·10)`. A
  duration of 0 or less never ends.
- **The textures** (`ResolveDecalTexturePath`, on the user side). A name
  that starts with `/` or `\` is a path. Otherwise it is a file in
  `/env/common/splats/` (a splat) or `/env/common/decals/` (a decal), with
  `.dds` added. An empty name is none.

### Who sees it (`CDecalBuffer`)

Each handle has a sight flag per army. Flags are only ever set.

- **At creation:**
  - A splat from a non-civilian army is seen by every army.
  - A splat from a civilian army is seen by its allies.
  - A decal, or a splat with no army, is seen by each non-civilian army
    that is the source's ally or whose recon can detect it; that army's
    allies see it too.
- **"Can detect"** is `ReconCanDetect(rect, y, LOSNow)`: some cell of the
  army's sight grid in the decal's bounds has line of sight now.
- **Each tick,** one army in turn (`tick % armies`, skipped if civilian)
  looks again. It checks:
  - every decal it doesn't see yet;
  - each splat within 10 ticks of its creation that has a lifetime.

  If it can detect one, it and its allies see it.
- **The focus army** sees a handle whose flag it has. An observer (focus −1)
  sees them all. A handle the focus comes to see is added to the user side;
  one it stops seeing is removed.
- **The end.** At its removal tick (`tick ≥` it) the sim destroys the
  handle, as `Destroy()` does. If the focus saw it, it is removed.

### The user side (`CDecalManager`, once a beat)

`CWldSession::DoBeat` calls `AddDecals`, `RemoveDecals` and then
`ProcessRemovals(tick)` for each beat.

- **Added.**
  - A decal's type is found by name. An unknown name is dropped.
  - A splat is always Albedo.
  - The cutoff is the record's LOD when it is over 0. Otherwise it is the
    diagonal of the bounds times 1 (`ren_DecalAlbedoLodCutoff` and
    `ren_DecalNormalLodCutoff`).
  - There is no near cutoff.
  - The alpha starts at 1.
  - The removal tick is the record's.
- **Removed.** A removed handle's removal tick becomes 1.
- **The fade.** Once the tick passes its removal tick (and that tick is
  over 0), the alpha steps toward 0 each beat:
  - a decal's by 0.2 (five beats);
  - a splat's by 0.03 (34 beats).

  At 0 it is gone.

### Drawing

- **Decals** draw as the map's (M212b), in the same Albedo and AlbedoXP
  passes, their alpha the LOD fade times their current alpha. The other
  types are kept but not drawn yet (M212d).
- **Splats** (`TSplats`) draw after the AlbedoXP decals:
  - **The quad.** Each is one quad (`CWldSplat::UpdateVertices`): the
    footprint's four corners at the terrain's height there, its UV 0 to 1
    across it (Moho's rectangle in its texture atlas).
  - **The alpha** (`SplatsPS`) is the albedo's alpha times the splat's
    alpha. The splat's alpha is the LOD fade, taken at its first corner,
    times its current alpha. It has no mask, and a splat past its cutoff
    isn't drawn.
  - **The light.** `CalculateLighting`, with no specular, by the terrain's
    normal, then the water tint.
  - **The state.** SrcAlpha / InvSrcAlpha into RGB only. Depth is tested
    LessEqual and not written. There is no culling, and the depth bias is
    −0.001: the flat quad shows over ground that rises within it.
  - **The budget.** At most 2500 a frame.
- **Low fidelity** (`LowFidelitySplat`: albedo only) isn't drawn: the
  engine draws the high-fidelity terrain.

## The engine

- **The sim.**
  - `sim::DecalSpec` (`sim/decal.hpp`) holds a record: splat or decal,
    type name, the two texture names, the corner, the turn, the size, the
    LOD, the removal tick, the army and the fidelity.
  - `make_decal_spec` does `CreateDecalFromTransform`'s arithmetic.
  - A decal or splat is still an `IEffect`, so its handle, `Destroy()` and
    trash bags work as before. It holds its spec (shared, never changed)
    and its sight flags. It ends at its removal tick (`ends_at`).
  - The sight rules live there too, over an interface to the armies
    (their count, civilians, allies, and "can detect"), so they can be
    tested alone.
  - SimState applies the creation rule as the bindings make a decal, and
    the tick's turn after `update_visibility`. "Can detect" reads the
    army's sight grid (16-unit cells) for vision. The engine has one
    vision grid, not Moho's separate one under the water.
- **The snapshot.** An `EffectRecord` carries the spec and the flags.
- **The renderer.**
  - `renderer::RuntimeDecals` (`runtime_decals.{hpp,cpp}`) is the user
    side. It needs no Vulkan and is unit-tested.
  - On each new tick it diffs the snapshot's decals against its own:
    - a record the focus sees and it hasn't got is added;
    - one it has that is gone or no longer seen is removed;
    - then it processes removals for each tick since the last.
    - A tick earlier than its last (a new game) clears it.
  - `renderer::RuntimeDecalRenderer` (`runtime_decal_renderer.{hpp,cpp}`)
    holds it, the decals' gathered triangles and the splat pipeline. The
    decal helpers M212b kept in `renderer.cpp` moved to `decal_math` for
    both.
  - The decal pass draws the runtime Albedo and AlbedoXP decals after the
    map's. It uses the same pipeline and index ranges from the terrain
    mesh, gathered when the set changes, into a per-frame index buffer.
  - The splat pass is a new pipeline. Its vertices are the corner, the
    terrain's normal there, the UV and the alpha, built each frame into a
    per-frame buffer. The shader is `SplatsPS` over the terrain's shared
    GLSL surface.
  - FA reads the normal buffer per pixel. The splat takes the terrain's
    normal at its corners, blended across it, and the normal maps per
    pixel. On flat or even ground that is the same.
- **The bindings.**
  - `CreateDecal`, `CreateSplat` and `CreateSplatOnBone` take Moho's
    arguments, resolve the army and make the spec.
  - `CreateSplatOnBone` uses the bone's world transform.
  - The splats return nothing.

## Tests

Unit tests:

- **The spec.**
  - The corner and turn from a heading and from a pitched bone.
  - The removal tick's floor.
  - The texture names.
- **The sight rules.** A fake set of armies:
  - splats from a civilian and a non-civilian;
  - a decal detected or not;
  - allies;
  - the turn and the splat's 10-tick window;
  - flags stay set;
  - civilian observers are skipped.
- **`RuntimeDecals`.**
  - Adds by the focus's flag, and an observer's.
  - A decal's fade takes 0.2 a beat and a splat's 0.03, from a removal or
    its tick.
  - An unknown type is dropped.
  - The default cutoff is the diagonal.
  - A new game clears it.

`--runtime-decal-test` (gate) runs on flat ground of its own, with textures
of its own, made by the sim's Lua:

1. **Centred.** A red `CreateDecal` 8 by 4, heading a quarter turn,
   covers its footprint about its position, and none of what a decal
   placed by its corner would.
2. **As the map's.** A runtime decal gives the same pixels as the map
   decal it corresponds to.
3. **Its lifetime.** A decal lasting 0.5 s is whole at 4 ticks. After its
   removal tick it fades by 0.2 a beat, and it is gone after five.
4. **`Destroy()`** starts the same fade.
5. **A splat.**
   - Its alpha is the albedo's, out to its corners. (It has no mask, but
     the decals' mask is whole but for its border texel, which only
     zeroes the lookup beyond a decal: within one the two agree.)
   - It takes no specular under a sun that glints.
6. **A splat's fade** is 0.03 a beat.
7. **Sight.**
   - An enemy's decal out of the focus army's sight isn't drawn, and
     shows once the focus army sees the spot.
   - An enemy's splat is drawn anyway.
8. **On a bone.** `CreateSplatOnBone` on a unit turned a quarter lays its
   splat turned with it.

## Left for later (M212d)

- Runtime and map Alpha Normals decals re-baked into the normal overlay as
  they come and go.
- Glow and Glow Mask decals (`TDecalsGlow`, `TDecalGlowMask`).
- Water Albedo, Water Mask and Water Normals decals.
