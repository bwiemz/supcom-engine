# M215g: Moho's Intel Grids — Design

**Status:** 2026-10-01. Roadmap Phase F tail: stealth and cloak fields, the underwater sight
grid, Moho's finer recon grids. All three are one change, Moho's per-army intel grids.

## Why

The engine kept one `VisibilityGrid`:

- 16-unit cells;
- a byte of flags per cell and army (Vision, Radar, Sonar, Omni, EverSeen);
- cleared and repainted every tick;
- vision blocked by terrain.

Moho's recon differs on each point. Sources: faf-re `ai/CAiReconDBImpl`, `sim/CIntelGrid`,
`entity/intel/CIntelPosHandle` and `CIntelCounterHandle`.

### Eight grids per army

| Grid | Cells | Holds |
|---|---|---|
| vision | 2 units | line of sight |
| water | 4 units | line of sight under the water's surface (WaterVision) |
| radar, sonar, omni | 4 units | those senses |
| RCI, SCI, VCI | 4 units | other armies' radar stealth, sonar stealth and cloak fields |

Vision and water exist only with fog of war on.

### Counts

A cell holds a signed byte. A source adds 1 over its circle when painted and takes it away when
rubbed out.

- A point is sensed where its cell isn't 0, and never off the grid.
- A rectangle is sensed where some cell in it is above 0.

### The raster (`CIntelGrid::Raster`)

- A flat circle: terrain blocks nothing.
- The radius is counted in whole cells, rounded down.
- Columns run from gx−r to gx+r, the last excluded. Each column's rows run from gz−leg to
  gz+leg, the last excluded, with leg = ⌊√(r²−dx²)⌋.
- So the circle leans a cell toward −z.

### Incremental repainting (`CIntelPosHandle`)

A handle repaints when:

- it is enabled, disabled or its radius changes;
- its entity moves a third of the radius, or keeps moving for 30 ticks;
- its entity stops;
- its entity dies.

### Detection (`ReconCanDetect`)

In order:

1. Off the playable area: nothing.
2. The army's own unit, or an ally's: everything.
3. Otherwise the army reads its own grids, and the grids of every army that calls it an ally,
   by the unit's layer:

   | Layer | Senses |
   |---|---|
   | Seabed, Sub (under the water) | water grid, sonar, omni |
   | Water | sight, radar, sonar, omni |
   | Land, Air | sight, radar, omni |

4. Then the counters (`ApplyReconCounters`), from the army's own counter grids:
   - omni beats them all;
   - RCI clears radar, SCI clears sonar;
   - the unit's own cloak, or VCI, clears sight;
   - only once out of sight does the unit's own radar or sonar stealth count.

### What the engine got wrong

- Stealth and cloak fields did nothing.
- WaterVision lit ordinary sight, so a ship's WaterVision saw tanks on the shore.
- Radar found submarines, and sonar found tanks.
- A unit's own stealth hid it even in sight.
- Terrain blocked sight.
- At 2-unit cells, its full repaint every tick would cost tens of milliseconds a tick.

## Sim

- **`map::IntelGrid` and `map::IntelGrids`** (new, `src/map/intel_grid.*`): Moho's grid and
  raster, and each army's eight grids. They replace `VisibilityGrid`.
- **Handles** (`src/sim/intel_sources.hpp`): each entity's painted intel, eight kinds. The
  kinds are Vision, WaterVision, Radar, Sonar, Omni and the three fields.
  - `SimState::sync_intel`, once a tick before the recon pass, brings every handle up to date
    by Moho's rules, then rubs out the handles of entities that are gone.
  - Units, script entities (VizMarkers) and `CreateVisibleAreaAtPoint`'s areas all paint
    through it.
  - A field paints into every army's counter grid but its owner's. The decompile shows no
    exception for allies.
- **Detection:** `SimState::recon_of(entity, pos, army)`, `recon_at(pos, army)` for a point and
  `recon_in(rect, y, army)` for a rectangle, all Moho's.
  - The tick's pass computes each army's recon of each unit once.
  - From that table: the blip cache, OnDetectedBy, OnIntelChange, LOSEver, the influence map's
    feed and detail, the jammers' blips and the snapshot's recon masks.
  - Weapons (a cloaked target needs omni), blip methods and decals ask directly.
- **Intel starts off,** as Moho's handles do.
  - Retail's `SetupIntel`, at `OnStopBeingBuilt`, switches it on: Vision, the blueprint's
    senses, and WaterVision only when the unit is wet. `OnLayerChange` swaps them after that.
  - Every unit has Vision and WaterVision, 10 unless its blueprint says otherwise (Moho's
    blueprint defaults).
  - `InitIntel` makes a new handle, off until `EnableIntel`.
  - The engine's half-cell "self-vision" is gone: an army's own units are its own.
- **No fog of war:** no vision or water grids, and sight everywhere. A cloak or a cloak field
  still hides a unit unless omni sees it.
  - The engine used to grant omni everywhere instead, which beat every cloak.
- **Saves** (snapshot version 5) keep each handle: where it was painted, its radius, its tick.
  On a load the grids are painted again from them, as Moho rebuilds its grids. The grids
  themselves are neither saved nor checksummed, as in Moho.

## Renderer

FA's render fog doesn't read the sim's grids. Its user side keeps vision circles (`VisionDB`)
and draws them as stencil volumes, then darkens what is outside them. There are two states,
with no explored ground. Its game side asks the live vision and water grids
(`UserArmy::CanSeePoint`).

For now the engine keeps its fog texture, fed from those grids:

- **Sight in the snapshot:** the world snapshot carries the watched army's sight (`SightMap`).
  It is the army's vision and water grids, and its allies', flattened to "seen or not". The
  app tells the history which army it watches.
- **Fog texture:** one texel per vision cell, seen or not, hard-edged. What isn't seen is
  darkened to 67%, FA's black at alpha 0.33. The old explored and radar levels, the blur and
  the desaturation are gone.
- **ReconView:** asks the sight map for projectiles (under the water, its water grid's), for
  remembered structures' spots and for effects.

## Tests

- **Unit (`test_intel_grid.cpp`):**
  - the raster: vision 20 at 2-unit cells is 296 cells, the default WaterVision 8, the lean;
  - counts and off-grid queries;
  - a rectangle's cells;
  - each layer's senses;
  - fields and omni;
  - own stealth only out of sight;
  - allies' grids, one way;
  - a moving source's repaints;
  - no fog of war.
- **Ported:** the existing tests that used the old grid (ReconView, decals, fog, intel toggles,
  VizMarkers, decal sight).

## Left

- **Detection cadence:** Moho runs detection for one army a tick (`tick % armies`, ReconTick).
  The engine still runs every army every tick: OnIntelChange's timing is what retail scripts
  and the tests see.
- **Fields and allies:** whether a field skips its owner's allies isn't settled in faf-re. The
  decompile skips only the owner.
- **Render fog:** FA's stencil volumes are not drawn yet. Their vision circles are
  interpolated per frame, and they darken units and effects too, not only the terrain. That
  is M215h.
- **Unused Moho mechanism:** `DelayedSubtractCircle`'s 30-tick linger has no known caller in
  faf-re.
