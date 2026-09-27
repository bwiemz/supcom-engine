# M215d: what counters the player's intel

M215a judged another army's unit by its grid cell's senses alone. Moho
counts, before a sense shows the player a unit, what counters it
(`CAiReconDBImpl::GetNewReconFor`, `ApplyReconCounters`; faf-re): so a
Cybran commander's cloak, a Seraphim unit's stealth and a submarine were as
plain to see as anything else. And a structure the player remembered
vanished the moment it died, wherever it was: the player learned of every
kill in the fog.

## The rules

- A **cloak** defeats vision: a cloaked unit in sight isn't seen. Omni
  defeats the cloak.
- **Stealth** defeats radar (radar stealth) or sonar (sonar stealth). Omni
  defeats it.
- **The water:** radar reaches no unit under it (the Sub and Seabed layers),
  sonar none out of it (only Water, Sub, Seabed).
- **Maybe dead:** a structure the player's army remembers (seen, then out of
  sight) that dies out of its sight stays drawn as last seen, its icon (and
  minimap dot) darkened (Moho's `DarkenRgbPreserveAlpha`: the tint halved),
  until the army has sight of the spot. A mobile unit's blip is simply
  dropped (M215a).

## The engine

- The sim already counts cloak and stealth for each army's recon of each
  unit at the end of its visibility update (for `OnIntelChange`):
  `SimState::entity_recon` now exposes it, and the snapshot turns it into two
  masks per unit, `EntityRecord::los_now` and `detected` (bit per army),
  adding the water rule. `ReconView` judges units by them instead of by the
  grid; projectiles still by the grid.
- `ReconView` keeps a remembered structure's record as last seen, and its
  `ghosts()` when it's gone from the world unseen; the unit, icon and
  minimap renderers draw them, the icon and dot darkened (`maybe_dead`).

## Tests

`[recon]` unit tests: masks over cells; ghosts (kept through radar, gone
when seen, forgotten with a new focus army).

`--counter-intel-test` (gate), SCMP_009, ARMY_2's engineers (one cloaked,
one radar-stealthed, one held on the seabed, one plain) and power
generator; ARMY_1's power generator with radar, sonar and omni to switch;
sight by scrying:

1. In sight, the cloaked engineer is hidden; the plain one drawn.
2. Cloaked, on radar: a blip.
3. Cloaked, under omni: drawn.
4. Stealthed: hidden on radar, a blip under omni (the plain one, seen
   before, a blip on radar).
5. On the seabed: hidden on radar, a blip on sonar.
6. The power generator, seen, then out of sight, destroyed: still drawn, its
   icon darkened.
7. Its spot seen: gone.

## Left for later

- Stealth and cloak **fields** (a generator's radius stealthing others), and
  the counter-intel grids Moho paints for them.
- The underwater line-of-sight grid (Moho sees a submerged unit only by
  `WaterVision`; the engine's one vision flag sees it from the shore).
- Jammers' fake blips; the sim's own intel queries (AI, `OnIntelChange`)
  aren't given the water rule yet.
- A maybe-dead structure's frozen health bar.
