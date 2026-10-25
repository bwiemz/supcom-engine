# M211t: the unlit projectiles' technique, Flat

Ten retail blueprints name `ShaderName = 'TMeshNoLighting'`, which Moho's
shader dictionary resolves to `Flat`; `mesh_technique` didn't know it, so
they drew as `Unit`, lit by the sun with its highlight, and every part of
their albedo drawn, transparent or not. Retail `mesh.fx` is read for the
formulas and states, never copied; FAF's data and technique are the same.

They are projectiles, among them those of:
- the Cybran Medusa (URL0103, `CIFGrenade01`), Viper (URL0111,
  `CIFMissileTactical01`, and its split `CIFMissileTacticalSplit01`) and
  torpedo ambusher (XRB2308, `CANKrilTorpedo01`);
- the mesh blueprints `torpedo_default_mesh`, `missile_default_mesh`,
  `bomb_default_mesh`, `missile_cruise01_mesh` and
  `missile_antiair_cybran01_mesh`, used by the Aeon anti-torpedo flares
  (frigate, destroyer, sonar, Tempest, submarine hunter, missile ship), the
  Wagner's and Monkeylord's torpedoes and the Aeon strategic bomber's bomb.

## The technique

`Flat` has one lane: `FlatVS` and `FlatPS`. The albedo times the instance's
colour, unlit; alpha `f * albedo.a` (the fraction complete, 1 for a
projectile), alpha-tested over `0x7F`. Opaque, colour only, depth tested and
written; a depth stage (`DepthClip`: the shadow is cut by the albedo's
alpha), a reflection, before the water. In the reflection its alpha is 0.5,
which the test passes.

## The engine

- `mesh_technique` maps `Flat` (`TMeshNoLighting` resolves first).
- `mesh_frag` draws it before the lit techniques: the albedo by the
  instance's colour (white for a prop, as for the others), discarded at or
  under `0x7F`, writing no glow; reflected without the test.
- The shadow pass cuts it by the albedo's alpha, as `NormalMappedAlpha`.

## Tests

- `--effect-mesh-test` Test 24, under Tests 8-11's sun: a white `Flat` plate
  shows its army's colour unlit; at albedo alpha 64, or 40% built, it isn't
  drawn.
- `--clipped-shadow-test` Test 1: a half-clear `Flat` plate casts half a
  shadow.
- `test_mesh_technique.cpp`; `--dispatch-matrix-test`.
- The plates' blueprints gain six structures.

## Left

`Explosion`, `GlassAlpha` and the rest of `tools/shader_names_baseline.txt`.
