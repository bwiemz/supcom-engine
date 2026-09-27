# M214a: FA's beams

The engine drew every beam (a build beam, a weapon's laser, a beam fixed to
a unit) as a flat coloured rectangle in the overlay: pale blue, orange-red,
whatever the placeholder chose. FA draws a beam from its `BeamBlueprint`
(`TextureName`, `Thickness`, `StartColor`/`EndColor`, `Length`, `UShift`/
`VShift`, `RepeatRate`, `Blendmode`, `LODCutoff`) as a textured strip
facing the camera, its texture scrolling, blended by one of particle.fx's
`TBeam` techniques. faf-re's `BeamRenderHelpers` (`EmitInterpolatedBeamQuadVertices`)
and particle.fx's `BeamVS`/`BeamPS` give the rules; retail's files are read
for them, never copied.

## The rules

- **Ends.** A beam runs from its start to its end, both world points:
  - `AttachBeamEntityToEntity`/`CreateBeamEntityToEntity`: one bone of one
    entity to one bone of another;
  - a beam emitter attached to an entity (`CreateBeamEmitter` +
    `AttachBeamToEntity`), or `CreateAttachedBeam`: from the bone, along
    its +Z, the blueprint's `Length` (the latter's own length argument);
  - a collision beam's (a weapon's): Lua attaches the emitter to the
    collision beam's bone 0, its start; `SetBeamFx` ties its end to the
    beam's end (the far end the sim finds, bone 1).
- **The strip.** Four vertices: the start and end, each pushed `Thickness`
  either way along `cross(view axis, beam axis)` (so the strip is
  2·`Thickness` across and faces the camera). Colour `StartColor` at the
  start, `EndColor` at the end.
- **UVs.** U is 1 on one side and 0 on the other; V is 0 at the start and
  `RepeatRate × length` at the end (1 when `RepeatRate` is 0); both scroll,
  `U + UShift·time`, `V + VShift·time`, FA's `time` in ticks (the tick plus
  the frame's interpolant). The texture wraps.
- **Shading.** `texture × colour`.
- **Blend.** `Blendmode` 0 `ALPHABLEND` (SrcAlpha, InvSrcAlpha; RGBA),
  1 `MODULATEINVERSE` (Zero, InvSrcColor; RGB), 2 `MODULATE2XINVERSE`
  (InvDestColor, InvSrcColor; RGB), 3 `ADD` (One, One; RGBA), 4
  `PREMODALPHA` (One, InvSrcAlpha; RGBA): D3D9 blends alpha by the same
  factors, so where a technique writes alpha a beam adds to the frame's
  glow (M211e: bloom). Depth tested, no depth write, both faces.
- **LOD.** Past the blueprint's `LODCutoff` from the camera (when it has
  one) a beam isn't drawn.
- **Intel.** As M215b: where the player's army sees either end.

## The engine

- `EffectRecord` carries each beam's world start and end (or start and
  direction, for a length the blueprint gives), found at capture from the
  sim's bone poses (`Unit::bone_world_position`/`bone_world_forward`), and
  a collision beam's own ends.
- `BeamBlueprintCache` reads `BeamBlueprint { … }` files as
  `EmitterBlueprintCache` reads emitters.
- `BeamRenderer`: the strips built on the CPU each frame, one pipeline per
  blend mode, drawn after the particles; the overlay's placeholder
  rectangles go for every beam it draws.
- The render-state dump lists the beams (blueprint, ends, width, colours,
  blend).

## Tests

`--beam-render-test` (gate), SCMP_009, ARMY_1's engineers hung 3 above the
ground (so their beams clear it), joined by beams of the test's blueprints
(a white texture):

1. A beam from A to B runs between them, 1 thick, additive, red to blue.
2. V spans length × RepeatRate (24 × 0.5 = 12); ten ticks scroll U by
   UShift × 10 and V by VShift × 10.
3. A beam emitter on C reaches the blueprint's Length along C's facing.
4. A collision beam's emitter runs from its start to its far end, with no
   overlay placeholder.
5. A beam past its LODCutoff isn't drawn.
6. In the frame: the additive beam adds red near its start and blue near
   its end; under the MODULATEINVERSE one the ground goes black.
7. An engineer building draws retail's build beams.
8. ARMY_2's beam in the fog isn't drawn; with the fog off it is.

`CreateAttachedBeam` (a texture, not a blueprint) has no retail caller and
keeps the overlay's placeholder.

## Left for later

- Two-texture beams (`TBeam_TwoTexture`: none of retail's blueprints name a
  second texture).
- Trails (`CreateTrail`: particle.fx's `TrailVS`), and the rest of M214.
