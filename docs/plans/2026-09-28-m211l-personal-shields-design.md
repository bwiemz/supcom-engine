# M211l: FA's personal shields

## Why

M211k drew the bubble shields. A personal shield has no bubble: retail's
`UnitShield` swaps its owner's mesh for the blueprint's `OwnerShieldMesh`
(`Owner:SetMesh(OwnerShieldMesh, true)`) and back when it goes down. That
mesh is the unit's own model, drawn with mesh.fx's `PhaseShield`
technique (or, for the Seraphim SCU, `SeraphimPersonalShield`). The
technique draws the unit, then a translucent electric shell just outside
it.

The engine draws the swapped mesh already, but as a plain unit: it doesn't
know the two techniques, so the shell never shows.

Seven retail units use them:
- the UEF, Aeon and Seraphim ACUs (PhaseShield, from an enhancement);
- the UEF SCUs UEL0301 and UEL0303;
- the Aeon UAL0202 (the Obsidian, shield built in) and SCU UAL0301;
- the Seraphim SCU XSL0301 (SeraphimPersonalShield).

## The rules (mesh.fx, faf-re)

- **PhaseShield (High and Medium):**
  - P0 is the unit's NormalMappedPS, opaque: exactly the Unit technique.
  - P1 is `PositionNormalOffsetVS(0.05)` with `PhaseShieldPS`: SrcAlpha,
    InvSrcAlpha, RGBA, cull CW, and the default depth state (test
    LessEqual, write on).
- **SeraphimPersonalShield:** P0 is `UnitFalloffPS` (the Seraphim
  technique), and P1 is `SeraphimPhaseShieldPS`, which reads the secondary
  texture where PhaseShieldPS reads the lookup.
- **Stages:** DEPTH + REFLECTION + PREWATER + PREEFFECT, a unit's: before
  the water, reflected, casting a shadow.
- **The shell's offset:** `position += normal / transPalette[bone].w *
  0.05`. A skinned instance (a unit) carries its uniform scale in the
  palette's w, so the shell is 0.05 world units out whatever the unit's
  size (HardwareMeshBatch).
- **The shaders:**
  - PhaseShieldPS samples the lookup three times at scrolled, scaled
    copies of uv0: (uv·0.5 + age·(0.005, 0.02)), (uv·4 + age·(−0.008,
    0.008)), (uv·0.01 + age·(−0.0018, 0)).
  - electricity = lookup.r × lookup2.b.
  - The colour is (0.5, 0.5, 1, 1) + electricity, times (lookup3.ggg,
    min(lookup3.g, 0.65) + electricity).
  - The age is `time - material.x`, in ticks.

## The engine

- **Mesh cache:** techniques PhaseShield (25) and SeraphimPersonalShield
  (26). Neither is post-water or post-effect, and both have a depth stage.
- **Renderer:**
  - Pass 0 draws with the mesh pipelines and the base technique pushed:
    Unit (0) or Seraphim (4). The mesh shaders are unchanged, and the
    shadow pass pushes the base as well.
  - Pass 1 draws with the shield shaders, in a sixth state: blended RGBA
    with depth written.
- **The shield shaders:** the offset shell (0.05 / the model's scale in
  mesh space), and PhaseShieldPS / SeraphimPhaseShieldPS.

## Tests

- **Unit:** the names, stage, depth stage and passes.
- **`--shield-render-test`** gains Test 5: the Obsidian's shield is up at
  spawn. It draws in two passes, and the shell changes the frame against
  the plain unit mesh. The shield down restores its blueprint mesh, drawn
  once.
