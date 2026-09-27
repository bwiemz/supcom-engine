# M211h: FA's rigid skinning, and hidden bones

Two things about how a mesh follows its bones.

**Skinning.** An SCM vertex names four bones, and the engine blended them a
quarter each (M65). FA's vertex shaders use the first alone
(`ComputeWorldMatrix(anim.y + boneIndex[0], ...)` in every mesh.fx vertex
shader). Of the 1,075,696 vertices in retail's 638 unit LOD0 meshes, 507,558
name one bone four times, where the two agree, and 568,138 name a bone
then three zeros: `[b, 0, 0, 0]`. There the engine moved a vertex by a
quarter of its bone and three quarters of the root, in 385 of the 638
meshes. At rest every skinning matrix is the identity and nothing shows;
animated, a turret aiming or a leg walking moved a quarter as far as it
should and sheared toward the root. The engine skins by the first bone,
weight 1, as FA does.

**Hidden bones.** `Unit:HideBone(bone, affectChildren)` hides a bone and,
with the flag, its subtree. The sim has recorded it since the bindings were
written, but the renderer drew everything: the UEF commander's upgrade
parts (`Right_Upgrade`, `Left_Upgrade`, `Back_Upgrade_B01`, hidden at
creation until built), a UEF structure under construction (bone 0 hidden
until its build cube has risen), and every other part a script hides.
Moho's `HardwareMeshBatch` parks a hidden bone's palette entry: no rotation
or scale, at y = -1000 (`kHiddenBoneDepth`), which collapses its geometry to
a point far below the world. Its shadow goes with it.

## The engine

- The SCM parser gives a vertex's first bone weight 1, the rest 0.
- `EntityRecord::hidden_bones`: a 64-bit mask of the unit's hidden bones
  (the renderer draws at most 64), captured from the sim each tick.
- The unit renderer parks a hidden bone after interpolating the pose: its
  skinning matrix becomes a zero 3x3 at (0, -1000, 0). The shadow pass
  reads the same matrices.

## Tests

`--skinning-test` (gate), on a two-bone plate of the test's own (a root, and a
child the plate's vertices name first: `[1, 0, 0, 0]`):

1. **Rigid.** Turned 45° by a rotator on its child bone, the plate's middle
   row widens by √2, as a square turned to a diamond; blended a quarter each,
   it turned about 11° and shrank.
2. **Hidden.** Its child hidden, the plate is gone; shown again, back.
   Hiding the root with its children hides it too.
3. **The commander.** A UEF ACU's snapshot lists its upgrade bones hidden;
   drawn, it differs from the same ACU with them shown.

## Left for later

- Entities other than units: Moho's `Entity` has no `HideBone`; the engine
  matches.
