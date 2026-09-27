# M215b: effects, beams, shields and clicks through the player's intel

M215a hid another army's units and projectiles from what the player's
intel doesn't see. Their effects still gave them away: the particles of a
factory building in the fog, a weapon beam across it, a shield bubble, a
click that could target a unit no one could see. Moho gates each (faf-re
`CEfxEmitter`, `CEfxBeam`, shield.lua); this milestone ports them.

## The rules

| What | Shown | Moho |
|---|---|---|
| an emitter with `EmitIfVisible` (2332 of retail's 2724) | emits only while the player's army has line of sight at it; its life runs on meanwhile | `CEfxEmitter::IsVisible`, `CanSeeCam`: `ReconCanDetect(pos, LOSNow)` |
| an emitter with `CreateIfVisible` (172) | made only if seen as it is made; else never | `ProcessLifetime` destroys it |
| a beam (an effect's, or a weapon's collision beam) | where the player's army sees either end | `CEfxBeam::CanSeeCam`, `BeamIsVisible` |
| another army's shield | where the player's army sees it | shield.lua `SetVizToEnemies('Intel')` |
| a click's target (right-click, order modes) | only a unit the player's intel shows: seen, a blip, or remembered | the user side picks among its own entities |

Line of sight is the focus army's, whoever made the effect (Moho doesn't
ask the effect's army), so the player's own and allies' effects show where
their units stand (their eyes are always on them). An observer sees
everything. The engine's placeholder overlay (an effect's dot or light, a
beam's line) follows the same rules.

Moho checks each emitter every 5 ticks and against each camera (an emitter
off screen doesn't emit); the engine checks every frame, and not the camera.
The gate is on emission only: particles already emitted live out their
lives, seen or not, as Moho moves each into the world's particle buffer as
it's emitted (`CEfxEmitter` pushes to `Sim::GetParticleBuffer()`), apart
from its emitter.

## Found on the way

- Retail's shield is up while it wears its mesh: shield.lua's `TurnOn` and
  `IsOn` are script states (`CreateShieldMesh`, `RemoveShield`), never the
  engine's. The snapshot counted a shield as up only by the engine's flag,
  so the overlay drew no retail shield; it now counts the mesh too.
- The renderer advanced particles only with a UI (its frame step lived in
  the UI's update), so an offscreen render never emitted any. The step is
  now the frame's.
- A new emitter starts where its entity is (it started at its offset from
  the world's origin for a frame).

## The engine

- `ReconView::sees_at` (M215a) answers every "does the player see this
  point" question; `sees_beam`, either end.
- `EmitterBlueprintData::create_if_visible`; `EmitterState::visible`;
  `ParticleSystem::set_recon` (and it remembers the CreateIfVisible effects
  it didn't make).
- `OverlayRenderer`: effects, beams, collision beams and shields ask
  `sees_at`.
- `InputHandler::set_recon`: clicks skip units the intel hides.

## Tests

`--effect-intel-test` (gate), SCMP_009, on dry ground away from the starts:
ARMY_2's engineers ("seen" under scrying, "fog" 40 east, "fog2" beside it)
and T2 shield generator; ARMY_1's engineer and power generator (a radar).
A steady emitter (retail's `aeon_build_01`, copied twice, the second copy
`CreateIfVisible`) on each of "seen" and "fog".

1. The steady emitter emits on "seen", not on "fog".
2. The CreateIfVisible one is made on "seen", not on "fog".
3. The overlay marks the effect on "seen", not on "fog".
4. Scried, "fog"'s steady emitter emits; its CreateIfVisible one stays
   unmade.
5. A beam between "fog" and "fog2" doesn't draw; one from "seen" to "fog"
   does.
6. A light and an attached beam on "fog" don't draw; on "seen" they do.
7. The shield draws scried, not in the fog.
8. A right-click or a capture order at "fog" doesn't pick it; on radar it
   does.

A weapon's collision beam shares the beams' rule (`ReconView::sees_beam`,
unit-tested); retail's beam weapons are naval or anti-missile, too much to
stage here, so its wiring has no integration test.

## Left for later

- Stealth and cloak, jammers' fake blips, `MaybeDead`, radar above water
  and sonar below, Moho's finer grids (M215c).
- The shield's own mesh (the dome, with its technique), rather than the
  overlay's ring.
- Decals and splats (their army's visibility).
- Moho's camera test for emitters, and its 5-tick recheck.
