# M215a: units seen through the player's intel

The renderer drew every army's units and projectiles wherever they were,
fog or no fog: the player's view was a map hack. FA shows another army's
unit only through the player's intel, as Moho's recon database
(`CAiReconDBImpl`, `ReconBlip`, `UserUnit::UpdateVisibility`; faf-re)
decides. See the memory note moho-recon-semantics for the full rules; this
milestone ports the ones for units and projectiles.

## What the player's army sees

The player's army is the UI's focus army (`SetFocusArmy`; -1 an observer):
the window hands it to the renderer each frame, and a multiplayer peer's is
its own command source's army. Its senses at a point are its grid cell's:
**LOS** (vision now), and **detected** (vision, radar, sonar or omni). Moho
ORs allies' senses in; the sim already shares allied vision into each
army's grid, so the player's cells hold its allies' senses too.

| Entity | Drawn |
|---|---|
| its own and allied units | as themselves |
| another army's **mobile** unit | its mesh while in LOS; else, while detected, a **blip**: an icon only; else nothing |
| another army's **structure** | once seen (LOSEver), its mesh for good, **frozen** (pose, fraction complete) as last seen while out of LOS; before that, while detected, a blip |
| another army's projectile | only in LOS |
| props, wrecks | always |
| everything, with no focus army (an observer), no visibility grid, or `--no-fog` | as itself |

A blip's icon draws at any zoom, having no mesh: the blueprint's own in the
army's colour once the unit has been seen (LOSEver); before that a generic
one (structure, air, naval, else land) in GameColors' `UnidentifiedColor`
(`FF808080`). A mobile unit that leaves every sense loses its LOSEver (Moho
deletes its blip), so it comes back unidentified. Radar, sonar and omni
alone never show a mesh. Health bars, selection rings and work beams show
only in LOS (a remembered structure's health isn't known); minimap dots
follow the icons. The engine's death flash, a stand-in for a death's
effects, shows as Moho shows an emitter: the player's own and allies'
anywhere, another army's in LOS. Camera shakes stay unfiltered: Moho queues
every `ShakeCamera` to the sync (faf-re `func_ShakeCamera`). With no fog of war (the lobby's "none"), the sim's grid
has vision everywhere, and everything shows. An observer's fog texture is
clear.

## The engine

- `EntityRecord::is_mobile` (`Unit::is_mobile`, from the motion type) and
  `ArmyRecord::allies` (a mask of the armies it's allied with).
- `ReconView` (renderer): once per tick, for the focus army, each other
  army's unit's and projectile's sight (hidden, blip, identified blip,
  seen, remembered) and each remembered structure's frozen pose and
  fraction; `Renderer` keeps one and hands it to the unit, icon, overlay
  and minimap renderers (null: everything seen). A new focus army, or a new
  scene, forgets what was seen.
- `GameColors::unidentified_color`.
- `ReconView::sees_at`: an effect's visibility (the death flash's).
- `lua::focus_army` / `lua::set_focus_army`; `FogRenderer::stage_clear`.

## Tests

`[recon]` unit tests: `ReconView`'s rules on hand-made snapshots.

`--unit-intel-test` (gate), SCMP_009, the player ARMY_1, on dry ground away
from the starts: ARMY_2's two engineers ("near", and "far" 40 east) and power
generator, ARMY_1's power generator as a radar, ARMY_3's engineer (made an
ally) with one of ARMY_2's beside it; the engineers are damaged. Sight is
scrying over "near" and the power generator, which the terrain can't block.
Draws are read from the render-state dump; icons, health bars and minimap
dots by the unit's projected position.

1. No sense: none of ARMY_2's draws: mesh, icon, health bar or minimap dot.
2. The ally's engineer draws, and ARMY_2's in its sight.
3. In sight: "near" and the power generator draw, "near" with its health
   bar and dot; "far" doesn't.
4. A shell of ARMY_2's draws in sight, not on radar alone.
5. Radar only: "near" (seen) is a blip in its army's colour, "far" one in
   `UnidentifiedColor`, icon and dot alike; no health bars.
6. The power generator is remembered, in its last pose in sight.
7. With no sense, the power generator, turning, still draws as seen;
   "near" is gone.
8. On radar again, "near" is unidentified.
9. Seen again, the power generator draws as it is (turned).
10. An observer sees everything.
11. With the fog off, everything draws.
12. A death flashes in sight (ARMY_2's engineer beside the ally's), not in
    the fog ("far").

Test 2 also has a shell of ARMY_3's hanging out over the fog: an ally's
shows anywhere.

## Left for later (M215b)

- Particles and emitters (LOS at the emitter), beams (LOS at either end),
  shields (Intel for enemies).
- Jammer blips (`JammerBlips`, `JamRadius`) and known fakes; stealth and
  cloak (the sim's blip cache already weighs them); `MaybeDead` (a
  remembered structure destroyed out of sight stays until the spot is
  seen); frozen health on remembered structures.
- Radar only above water and sonar below; Moho's finer grids (LOS in 2-unit
  cells, radar in 4; the engine's are 16) and the underwater LOS grid; the
  recon tick's one-army-per-tick lag.
- Picking and rollover (the input handler reads the live sim, so a hidden
  unit can still be clicked); the UI's unit queries.
- FA's strategic icon textures (the icons are still procedural shapes).
