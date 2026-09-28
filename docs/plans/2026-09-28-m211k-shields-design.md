# M211k: FA's shields

## Why

The engine draws a shield as a flat circle outline, an overlay of its own.
FA draws it as a mesh: shield.lua gives the shield entity its blueprint's
`Mesh` (`SetMesh`, `SetDrawScale(Size)`, `SetParentOffset`), adds an entity
carrying `MeshZ`, the depth fill, and on each hit an entity carrying
`ImpactMesh`, turned to face the shot, for five seconds. Each mesh's
`ShaderName` names a mesh.fx technique: `ShieldUEF`, `ShieldCybran`,
`ShieldAeon`, `ShieldSeraphim`, `ShieldFill`, `ShieldImpact`,
`CybranShieldImpact`.

None of this reaches the screen. The renderer draws only units', props' and
projectiles' meshes, and the sim puts an attached entity at its parent's
origin, ignoring the parent offset.

The rules come from faf-re:
- `MeshRenderer::Render` and `WRenViewport::Render`: the stages and the
  frame's order;
- `MeshRenderer::Batch` and `MeshBatchKeyLess`: SortOrder;
- `HardwareMeshBatch::FillBatch` and `UserEntity::UpdateEntityData`: the
  instance's time, parameter and transform;
- `Entity::CalculateAttachedTransform` and `Unit::GetBoneWorldTransform`:
  where an attached entity sits;
- `CD3DEffectTechnique::FinalizeMissingImplementations`: fidelity.

The shaders' formulas are mesh.fx's (read for them, not copied).

## The rules

### Where a shield is
- An attached entity sits at its parent bone's world transform composed with
  its parent offset: position = bone position + bone orientation × offset,
  orientation = the bone's.
- A unit's bone -1 is its centre: its position plus orientation ×
  (0, SizeY/2, 0). Another entity's bone -1 is its own transform.
- So a bubble shield (attached at -1, offset (0, ShieldVerticalOffset, 0))
  is centred half the unit's height up, plus the offset. This is the sim's
  position, and so the collision sphere's centre too.
- Its scale is the draw scale alone: a mesh blueprint's UniformScale isn't
  applied. The shield sphere's radius is 0.5, so a shield of Size s draws
  with diameter s.

### Which entities draw
- Any entity with a mesh draws: shields, and script entities (`Entity{}`)
  given one. They aren't reflected in the water.
- Visibility: a shield or script entity of the focus army or an ally always
  shows. An enemy's shows where the focus army sees (`SetVizToEnemies
  ('Intel')`, the default for script entities).

### The instance
- material.x is the tick its mesh instance was made (again on SetMesh).
  The shaders take the age, `time - material.x`, in ticks.
- material.y is the technique's parameter. For the shields (and the fill)
  it is PARAM_FRACTIONHEALTH: the entity's health over its max health, 1
  with no max. The impacts' is unused.

### Stage and order
- All seven techniques are POSTWATER + POSTEFFECT. Moho draws that stage
  last among the meshes: after the water and the above-water beams,
  particles and trails, before the refracting effects.
- Meshes draw in their blueprint's SortOrder, smallest first, with no
  distance sort: impacts (500), then the fill (999), then the shield (1000).
- There is no depth stage: shields cast no shadow.
- With no _HighFidelity variant, High fidelity draws the _MedFidelity one.

### The techniques (medium fidelity)

| Technique | Passes | Blend | Cull | Depth |
|---|---|---|---|---|
| ShieldUEF | FourUVTexShiftScaleVS, ShieldPS | SrcAlpha, InvSrcAlpha, RGBA | none | test, no write |
| ShieldCybran | FourUV…VS, then ShieldPositionNormalOffsetVS (0.01); ShieldCybranPS(0.17) | SrcAlpha, InvSrcAlpha, RGBA | CW | test, no write |
| ShieldAeon | ShieldNormalVS, ShieldAeonPS | SrcAlpha, InvSrcAlpha, RGBA | CW | test, no write |
| ShieldSeraphim | ShieldNormalVS, ShieldSeraphimPS | SrcAlpha, One, RGB | CW | test, no write |
| ShieldFill | FlatVS, ShieldFillPS | none written | CW | test and write |
| ShieldImpact | ShieldImpactVS, ShieldImpactPS(2, 0.2) | SrcAlpha, One, RGBA | CW | test, no write |
| CybranShieldImpact | ShieldImpactVS, CybranShieldImpactPS(6, 0.15, 4.5) | SrcAlpha, InvSrcAlpha, RGBA | none | test, no write |

- Blending RGBA blends alpha, the frame's glow, by the same factors (D3D9
  without a separate alpha blend), so shields glow a little.
- The fill writes only depth: drawn just before its shield, it hides the
  sphere's far side. That matters most for UEF's, which culls nothing.

## The engine

- **Sim:** `follow_attachments` places a child at Moho's attached transform.
  An attached child's own bone is taken as its origin, which is exact for
  shields and script entities (no blueprint).
- **Mesh cache:** the seven techniques; the mesh blueprint's SortOrder.
- **Unit renderer:** instances for shields and script entities with a mesh;
  the parameter by technique; groups sorted by SortOrder (stable, so equal
  keys keep today's order).
- **Renderer:**
  - the shield shaders (their own vertex and fragment shaders, the mesh
    layout);
  - five pipelines (blend, blend unculled, additive RGB, additive RGBA,
    fill);
  - a post-effect mesh pass after the trails;
  - shields left out of the shadow pass.
- **Overlay:** the circle outline goes.

## Left

- **M211l:** the personal shields: PhaseShield and SeraphimPersonalShield,
  the owner's mesh with a second, offset pass.
- The low fidelity variants.
- Script entities' viz settings beyond the defaults (Never, and Always
  outside the playable area).
- A child's own bone in an attachment.

## Tests

- **Unit:**
  - the techniques' names, stage, parameter and passes;
  - SortOrder ordering;
  - the attached transform (bone -1 of a unit and of an entity, an offset
    turned with the parent).
- **`--shield-render-test` (gate), SCMP_009:**
  1. A UEF shield generator's shield sits at the unit's centre plus its
     offset, and draws with its fill, fill first.
  2. It shows: its disc is bluer than with the shield off, and the far side
     shows once the fill is gone.
  3. A hit halves its health and adds an impact facing the shot; the
     shield's parameter is its health.
  4. Cybran's, Aeon's and Seraphim's shields draw with their techniques.
  5. An enemy's shield out of the player's sight doesn't draw.
