# M214b: FA's trails

Scripts make a trail with `CreateTrail(entity, bone, army, blueprint)`: a
projectile's polytrail (`SinglePolyTrailProjectile` and its kin, offset along
the shell with `OffsetEmitter(0, 0, PolyTrailOffset)`), and an aircraft's
wingtip contrails (`CreateContrails`, offset with `SetEmitterParam('POSITION_Z')`).
The engine tracked the effect but drew only the overlay's placeholder dot.
FA draws a ribbon along the path the point has travelled, `TrailLength` ticks
long, its two textures blended by one of particle.fx's `TPolyTrail`
techniques. The rules come from faf-re's `CEfxTrailEmitter` (`OnTick`, `Tick`,
`CanSeeCam`, `ProcessLifetime`), `SWorldTrail`, `PackTrailSegmentQuadVertices`,
`UploadPendingTrailsIntoWorkItem`, `RTrailBlueprint`'s type info, and
particle.fx's `TrailVS`/`TrailPS`. Retail's files are read for them, never
copied.

## The rules

- **Blueprint** (`TrailEmitterBlueprint { … }`; the reflected names):
  - `Lifetime` 0, `TrailLength` 0 (ticks), `Size` 0 (half the ribbon's
    width), `SortOrder` 0, `BlendMode` 0, `LODCutoff` 100,
    `TextureRepeatRate` 0 (repeats a unit of distance), `EmitIfVisible` true,
    `RepeatTexture`, `RampTexture`.
  - Moho has no `UShift`/`VShift` for trails (six retail blueprints name
    them; they're ignored), and `ScaleEmitter` doesn't reach the width.
- **The point.** The trail's point is its entity's bone (the entity itself
  for -1), offset in the bone's frame. `OffsetEmitter(x, y, z)` adds to that
  offset; `SetEmitterParam('POSITION_X'/'_Y'/'_Z', v)` sets one part of it
  (both write the same params in Moho).
- **Segments.** Every tick the trail emits one segment:
  - from where the point was last tick to where it is now;
  - each end's tangent: the start's is the previous segment's direction (the
    segment's own on the first), the end's this segment's, so neighbours
    share the joint;
  - each end's texture coordinate: `TextureRepeatRate` × the distance the
    point has travelled so far.
- **Segments live on their own** in the world's particle buffer. They outlive
  the emitter (a shell's trail fades after it hits) and are dropped
  `TrailLength` ticks after their newer end.
- **Age.** A point recorded on tick k is born at time k + 1 on the render
  clock (ticks plus the frame's interpolant). This is the moment the entity,
  drawn between its last two ticks, reaches it.
  - Each end's age fraction is `t = (time − born) / TrailLength`.
  - Only fragments with `0 < t < 1` are drawn, so the ribbon's head follows
    the drawn entity along the newest segment, and its tail ends
    `TrailLength` ticks back.
- **The ribbon.** Four vertices per segment:
  - each end is pushed `Size` along `±cross(view axis, tangent)`;
  - the side with the negated tangent has V 1, the other V 0.
- **Shading.** `ramp(t, V) × repeat(V, distance coordinate)`:
  - the ramp texture clamps;
  - the repeat texture wraps.
- **Blend.** `BlendMode` picks the `TPolyTrail` technique, all RGB-only (no
  trail adds glow):

  | BlendMode | Technique | Blend |
  | --- | --- | --- |
  | 0 | ALPHABLEND | SrcAlpha / InvSrcAlpha |
  | 1 | MODULATEINVERSE | Zero / InvSrcColor |
  | 2 | MODULATE2XINVERSE | InvDestColor / InvSrcColor |
  | 3 | ADD | SrcAlpha / One |
  | 4 | PREMODALPHA | One / InvSrcAlpha |

  Depth tested, no depth write, both faces.
- **Emitting only when seen** (`EmitIfVisible`). A trail emits only while:
  - its point is within `LODCutoff` of the camera;
  - a 5-unit sphere around it is in the view;
  - the player's army has LOS there (Moho asks the army's recon DB for
    LOSNow at the point, whoever made the trail). It asks on the first tick
    and then every fifth, reusing the answer between.

  Unseen ticks are counted. When the trail is seen again, it first emits up to
  `min(count, TrailLength, 24)` of the missed segments (from the entity's
  position history). If it had to drop some, it starts afresh with no carried
  tangent.
- **Draw order.** Trails draw after the particles and beams. Those with a
  negative `SortOrder` (retail's underwater trails, −102) draw before the
  water, so it covers them.
  - Open question: faf-re's `RenderEffects` reads `≤ 0` for the underwater
    pass and `≥ 0` for the surface pass, which would draw `SortOrder` 0 twice.
    That hasn't been checked against the binary, so 0 draws once, above the
    water.
- **Lifetime.** A trail with `Lifetime` ≥ 0 stops emitting that many ticks
  after it appeared. (Moho destroys the effect; here the renderer just
  stops, since no retail trail has one: all are negative, so a trail ends
  with its entity, which the sim already does.) On its first tick the
  renderer has only one point, so it draws one segment fewer than Moho.

## The engine

- **Sim.**
  - `EffectRecord` carries a trail's point for the tick (`anchor`), found at
    capture from the sim's bone pose (`Unit::bone_world_point`: a point in a
    bone's frame, in the world) or the entity's own transform.
  - `OffsetEmitter` adds, and `POSITION_*` params write the offset.
- **Blueprints.** `TrailBlueprintCache` reads `TrailEmitterBlueprint` files
  as `BeamBlueprintCache` reads beams, both through one runner that captures
  the wanted kind and ignores the others.
- **`TrailRenderer`:**
  - Each new snapshot, it records each trail's point (keeping the last 25 for
    catch-up) and applies the emission rules. The segments it emits go to its
    own list.
  - Each frame, it builds the ribbons on the CPU from the list. Each segment
    end's `t` is found there, so the shader only tests and shades it.
  - It has a pipeline per blend mode and draws in two passes (under the
    water, then after the beams).
  - The overlay drops its placeholder dot for every trail it draws.
- **Render-state dump.** A `[trails]` section with one line per segment:
  effect, blueprint, ends, `t` at each, distance coordinates, size, blend,
  and pass.

## Tests

`--trail-render-test` (gate), SCMP_009. The test's own blueprints (white or
coloured textures) ride units that the test moves each tick:

1. A trail on a moving unit: one segment a tick, joining the recorded
   points. The newest segment ends at the unit's point, and its ends' `t` are
   those of the render clock; size and distance coordinates are the
   blueprint's. (1b) After a turn, the new segment's start carries the last
   segment's direction.
2. Segments go `TrailLength` ticks after their newer end; a trail that stops
   moving leaves nothing.
3. `OffsetEmitter(0, 0, z)` puts the point `z` ahead of the unit along its
   facing, and a second call adds to it. `SetEmitterParam('POSITION_Z')` sets
   it.
4. A trail on a bone follows the bone.
5. The trail outlives its unit; its segments fade out.
6. ARMY_2's trail:
   - once fogged, it keeps emitting until its next look (every fifth tick);
   - seen again, it waits for that look, then catches up
     `min(missed, TrailLength, 24)` segments along its real path, starting
     afresh;
   - (6b) out of view, its looks restart, so back in view it looks at once.
7. Past `LODCutoff` from the camera a trail emits nothing, and catches up
   when back in range. (7b) Likewise out of view.
8. In the frame:
   - an additive trail whose ramp runs red to blue adds red near its head
     (right to the ramp's clamped edge) and blue further back, keeping the
     ground's green, and adds nothing past its life or ahead of its head;
   - a MODULATEINVERSE trail darkens the ground;
   - a striped repeat texture stripes the trail along its distance.
   - (8b) Drawn halfway into a tick (interpolant 0.5), the head stops where
     the unit is drawn.
9. A negative `SortOrder` trail draws in the underwater pass: over water it
   shows dimmed, under a `SortOrder` 0 one's full colour.
10. (a) A drawn trail has no overlay dot, and a trail with no blueprint keeps
    its dot. (b) A retail Gauss shell's polytrail draws from retail's
    blueprint.
11. A `Lifetime` 3 trail stops emitting 3 ticks after it appeared.

## Left for later

- Particle emitters on bones (the overlay's dots for them, and bone frames
  for their offsets): the rest of M214.
- Whether Moho draws `SortOrder` 0 in both passes (see above).
