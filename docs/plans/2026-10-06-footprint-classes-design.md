# Footprint classes and occupation: design (roadmap item 4)

Date: 2026-10-06. Sources: faf-re `STIMap.cpp` (`OCCUPY_MobileCheck` 0x00564AB0, `OCCUPY_FootprintFits` 0x007209E0), `RRuleGameRules.cpp` (`FindFootprint`), `RUnitBlueprint.cpp` (`RUnitBlueprintPhysics::ComputeDerivedQuantities` 0x0051F260), `COGrid.cpp` (`ExecuteOccupy`/`ReleaseOccupy`), `Prop.cpp` (constructor and destructor), `CAiPathFinder.cpp` (`CanTraverseCell` 0x005AA710), `PathTables.cpp`; retail `mohodata.scd/lua/footprints.lua`.

## What Moho does

### Footprint classes

`/lua/footprints.lua` calls `SpecFootprints{...}` with 20 named classes. Each class has:
- `SizeX`, `SizeZ`: whole cells;
- `Caps`: LAND 1, SEABED 2, SUB 4, WATER 8;
- `MaxWaterDepth`, `MinWaterDepth`, `MaxSlope`;
- `Flags`: IgnoreStructures 1.

The table keeps spec order. A class's index is also its path cluster map.

### A unit's footprint

A blueprint's footprint starts as `Footprint.SizeX/SizeZ`. A missing size is filled from `SizeX`/`SizeZ` rounded up.

For a mobile unit (MotionType not None), the caps and flags come from the MotionType, not the blueprint:

| MotionType | Caps |
|---|---|
| Land, Biped | LAND |
| Amphibious | LAND\|SEABED |
| Hover, AmphibiousFloating | LAND\|WATER |
| SurfacingSub | SUB\|WATER |
| Water | WATER |
| Air | AIR |

If the caps include a ground cap (mask 0x0F), `FindFootprint` picks the class with exactly these caps whose size is nearest. Nearest means the smallest max(|dX|, |dZ|); the first in spec order wins a tie. The class then replaces the whole footprint: size, slope and depths. AltMotionType gets an alt footprint the same way, falling back to the main one.

A structure (MotionType None) takes its caps from its BuildOnLayerCaps. A seabed structure whose MaxWaterDepth is 0 gets FLT_MAX.

### Can a footprint stand at origin cell (x0, z0)?

Moho decides this on the 1-unit heightfield. The origin is `lrint(world − Size/2)`, and the footprint covers the vertices `x0..x0+SizeX` × `z0..z0+SizeZ`.

`OCCUPY_MobileCheck`:
- It returns 0 when the footprint is off the map or any vertex is blocking terrain.
- Otherwise it starts from the footprint's caps and narrows them:
  - WATER/SUB/SEABED go when `MinWaterDepth > water − highest vertex`;
  - LAND/SEABED go when `water − lowest vertex > MaxWaterDepth`;
  - LAND/SEABED also go when `MaxSlope ≠ 0` and the largest height step between neighbouring vertices exceeds `MaxSlope`. Rows are scanned, then columns, in world units.

`OCCUPY_FootprintFits` then narrows further (1x1 footprints go through `OCCUPY_Filter`):
- LAND/SEABED go when any terrain-occupation bit in the rect is set, unless the footprint has IgnoreStructures;
- WATER goes when any water-occupation bit is set.

### Occupation

`COGrid` keeps two bitmaps of 1-unit cells: terrain and water. `ExecuteOccupy(caps, rect)` sets bits:
- LAND, SEABED or SUB set terrain bits;
- WATER sets water bits.

`ReleaseOccupy` clears the same bits. Each also dirties the path clusters.

Who occupies:
- Structures occupy their footprint.
- A prop occupies its blueprint footprint at `lrint(pos − Size/2)`, but only if it has reclaim value (`ReclaimMassMax` or `ReclaimEnergyMax` > 0). It releases it in its destructor.
- 88 of retail's 336 prop blueprints have `Footprint.Caps = 3` (LAND|SEABED): buses, buildings, wrecks of scenery. 3 have Caps 0, and 244 have no footprint.

### The pathfinder

- It is an 8-way A*. A diagonal is taken only once both of its cardinals are; steps cost 1 and 1.414.
- It searches a hierarchical cluster map per footprint, within a CPU budget. A search that runs out returns the closest cell.
- `CanTraverseCell` runs MobileCheck and then FootprintFits for the unit's footprint. A unit on the Water layer drops SUB first.
- A non-idle search also rejects cells a mobile unit blocks.
- A goal-boundary probe adds IgnoreStructures.

## What the engine does (e1f275b3)

- `SpecFootprints` is a no-op.
- There is one grid of 2-unit cells: Passable, Impassable, Water or Obstacle.
- There are four move classes: Land, Amphibious (hover folded in), Water (by draft) and Air.
- Slope is one rule: a rise above 1.5 over a cell.
- Units path as points: no size, no clearance.
- Only finished STRUCTURE units mark cells (refcounted, freed on unregister). Props never block.
- MaxSlope, MaxWaterDepth and the footprint's caps are never read.

## Plan: four PRs

### PR 4a: footprint classes and each unit's footprint (data only)

- The blueprint loader runs `/lua/footprints.lua` before `Blueprints.lua`, as `RuleInit.lua` does. The file defines the caps and flag globals it is written with (`LAND = 0x01` … `IgnoreStructures = 0x01`). Retail's table and FAF's differ in one class: retail has WaterLand5x5, FAF WaterLand4x4.
- `SpecFootprints` stores the table in the `BlueprintStore` (`footprint_classes()`, in spec order, each with its index). The types are in `blueprints/footprint.hpp`: the store sits below the sim, which uses them.
- Each unit resolves its footprints at creation: Moho's MotionType caps table, `FindFootprint`, the AltMotionType fallback and the structure rule (`resolve_unit_footprints`). `Unit` exposes `footprint()`, `alt_footprint()` and `footprint_class()`; they are saved.
- Nothing moves differently yet. In particular, `footprint_size_x/z` stay the blueprint's: Moho overwrites a mobile unit's size with its class's, which PR 4c takes on with the pathing.
- Tests:
  - retail's table parsed through `SpecFootprints` (LuaPlus `|`);
  - nearest-size picks and ties, e.g. a 3x3 land unit → Vehicle2x2, a 4x4 → Vehicle5x5;
  - whole-class replacement;
  - fliers;
  - AltMotionType and its fallback;
  - structures.

### PR 4b: the occupation grid and `footprint_fits`

- **The grid.** `sim/occupancy.hpp` has `OccupancyGrid`: one cell a map cell, ground and water bits as Moho's `COGrid`, with off the map counting as occupied.
- **Who claims it.** `SimState::occupy_ground`/`release_ground` record each entity's claim (`GroundOccupant`: caps and rects).
  - **Releases:** a release clears the claim's rects, then any other occupant standing on those cells claims them again. Moho's grid, bits alone, leaves them cleared; keeping them makes the grid exactly what stands on it.
  - **Saves:** a save keeps the claims, and a load rebuilds the grid from them (state version 21).
  - **Immobile units** claim at spawn, finished or not (Moho's `ExecuteOccupyGround`): their `Physics.OccupyRects` (centre offset and half size; the quantum gateways), else their footprint. Ferry beacons and in-place upgrades don't.
  - **Props** worth reclaiming claim their footprint with their blueprint's `Footprint.OccupancyCaps`, as Moho's `Prop` does. 88 of retail's 336 prop blueprints have LAND|SEABED.
  - **Released** as the entity leaves the sim.
- **The fit test.** `map_caps` and `footprint_fits` are Moho's `OCCUPY_MobileCheck` (with `OccupancyCapsOfFootprintAt` for one cell) and `OCCUPY_FootprintFits`, exactly. `SimState::footprint_fits_at` is `SFootprint::FitsAt`.
- **Blocking terrain.** `Terrain::is_blocking_cell` is Moho's `STIMap::IsBlockingTerrain`: the map's last row and column, and terrain types `/lua/TerrainTypes.lua` marks Blocking. Those are read at map load.
- **Measured:** claims at a session's start are 22 on SCMP_009, 1,186 on SCMP_001, 16 on SCMP_015 and 108 on SCMP_024.
- Nothing paths by the grid yet (4c).
- **Tests:** Moho's narrowing rules one by one; a single cell against its own cell; the map's edge; occupied ground and water; IgnoreStructures; claims overlapping and released; a save restoring the grid; units' claims through `CreateUnit` (a footprint, a gateway's rects, none for a ferry beacon or a tank, freed on destruction).

### PR 4c: paths per footprint class

- Recommendation: keep the 2-unit search grid and its A*, but decide passability per footprint class. A 2-unit cell is passable for a class when the class's footprint fits, under Moho's rule, at the origin that centres it on the cell.
- This gives clearance (a 6x6 amphibious can't take a 2-unit gap) and the per-class slope and depth limits, at the current search's cost.
- Per-class passability is cached as bitmaps on first use, and updated for a rect when occupation changes. That is Moho's `DirtyClusters` in the engine's terms.
- Reachability components become per class, replacing the land/amphibious/draft keys.
- MoveClass maps onto the unit's class; naval draft is replaced by the class's MinWaterDepth. Air stays as is.
- Deviation from Moho: passability is sampled every 2 units, not every 1. A 1-unit gap that Moho's Vehicle1x1 could pass, the engine may not.
- A 1-unit search grid would need Moho's hierarchical clusters to stay fast on 81 km maps (16.7M cells). That is a later step if parity tests demand it.
- Tests: a big unit routes round a gap a small one takes; the slope and depth limits per class; a structure built in a corridor closes it for big units only where it should.

### PR 4d: units in each other's way, and verification

- `CanTraverseCell`'s mobile-blocker rule (`COGrid::UnitIsBlocked`), if the engine's steering doesn't already cover it. Check that first.
- Verification on retail maps: long four-AI games, the path and crowd integration modes, the bench, before and after.

## Risks

- **Performance:** per-class bitmaps cost `grid cells × classes in use` bits (an 81 km map at 2 units: 4.2M bits per class), and updating them on occupation is per rect.
- **Behaviour changes everywhere units move:** expect desync-free but different games. Goldens at frame 600 may move if a unit routes differently.
- **Blocking terrain:** `STIMap::IsBlockingTerrain` (0x00577F20) is true for the map's last row and column, and for a cell whose .scmap terrain type has `Blocking = true` in `/lua/TerrainTypes.lua`. Retail marks two types this way: Dirt09 and Lava01. The engine already parses the type layer (`ScmapData::terrain_types`, `Terrain::terrain_type`), so PR 4b reads the table's Blocking flags.
