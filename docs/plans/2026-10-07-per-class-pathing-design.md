# Per-class pathing (roadmap item 4c)

Units should path as Moho paths them: one search graph per footprint class
(`/lua/footprints.lua`, 20 classes in retail), with the class's size, caps,
depths and slope, and with structures folded in. The engine searches one
grid shared by every unit, as points, by layer and draft; a 12x12 sub and
a 1x1 tank see the same gaps.

Source: faf-re `Cluster.cpp`, `ClusterMap.h`, `BitArray2D.cpp`,
`PathTables.cpp`, `STIMap.cpp` (`OCCUPY_Filter`,
`OccupancyCapsOfFootprintAt`), `CAiPathFinder.cpp`, `AStarSearch.h`,
`CAiPathNavigator.cpp`, `Sim.cpp` (`AdvanceBeat`).

## What Moho does

**One cluster map a class.** `PathTables` makes, for each class (its index
its place in the spec), an occupation source and a `gpg::HaStar::ClusterMap`
over the map's cells (`heightfield - 1` each way), two levels deep, all
sharing one `ClusterCache`. A path cell is the footprint's corner,
`lrint(world - size / 2)`.

**Occupation windows.** Level-1 cluster `(cx, cz)` asks its source for a
9x9 window at `(8cx, 8cz)`: bit `(b, r)` set when the class may stand with
its corner at `(8cx + b, 8cz + r)`, every cell under it passing the
per-cell test (`OCCUPY_Filter`: off the map, blocking terrain, depths,
slope over the cell's edges, occupied ground unless IgnoreStructures,
occupied water). Neighbouring windows share their boundary line.

**Level-1 clusters** (`ClusterBuild(OccupationData)`):

1. Portals: each maximal open run along the window's four edges gives one
   node, at 4 if the run covers 4, else at a corner it touches, else at
   `(s + e + (e < 4)) / 2`; sorted by `x | z << 8`, duplicates dropped.
2. Costs: from each node a label-correcting search over the window (a FIFO
   ring; a cell that improves goes to the back), steps 1 and 1.41421354,
   a diagonal only when both of its sides are open. Each pair's cost is
   quantised against their octile distance, `max + 0.41421354 min`:
   `clamp(ceil(6 ln(cost / octile)), 0, 31)`, -1 unreachable. The
   pairs are a triangle, `(s < t)` at `s + t(t - 1) / 2`.
3. Nodes with no edge go.

**Level-2 clusters** (`ClusterBuild(SubclusterData)`): the 4x4 children's
nodes on the parent's boundary lines, sorted and deduplicated; from each, a
Dijkstra over the children's edges (dequantised, `octile * exp(q / 6)`,
from a 32-entry table); each earlier node's distance quantised again;
nodes with no edge go.

**The cache.** Level-1 payloads are keyed by their window, level-2 by
their children's content; identical clusters share one payload across every
class's map, and a payload goes when its last handle does.

**Dirtying.** Every cluster starts dirty. A structure or prop claiming or
releasing ground (`COGrid::ExecuteOccupy/ReleaseOccupy`) dirties the rect on
every class's map (`DirtyRect`); nothing is rebuilt then.

**Rebuilding.** Each tick, before the armies, `PathTables::UpdateBackground`
spends one budget (`path_BackgroundBudget`, 1000) across the maps in class
order: a map scans its dirty top-level clusters from where it left off
(`BitArray2D::AnyBitSet`) and rebuilds each, children first (z outer,
x inner); a cluster costs 10, and a build is refused only once the budget
is spent, so it may go negative. A search rebuilds what it reads on demand
from its own budget, so it never reads stale data.

**Search** (for 4c-2): one path queue an army (2500 a tick), A* over cell
corners at level 0 near the ends and cluster portals between, closest-cell
fallback, no smoothing; the navigator string-pulls ahead along the path.
Mobile units are never in the clusters; only repaths test them, at level 0.

## Where the engine differs, and why

- **Quantising without `log`.** `ceil(6 ln r)` is the least `q` with
  `r <= exp(q / 6)`, so the build compares the ratio against the same
  32-entry table it dequantises with. `logf` differs between the libms in
  its last bit; the table is the same bits everywhere.
- **The dirty rect.** Moho pads the rect by `{-1, -1, sizeX + 1,
  sizeZ + 1}` field-wise, which widens its high side. But cluster `cx`
  reads cells `[8cx, 8cx + 7 + sizeX]`, so a change at `x` reaches the
  clusters below it: with a 3-wide class and a change at x = 10, Moho's
  rect leaves cluster 0 (cells 0-10) clean. The port dirties exactly the
  clusters whose windows read a changed cell, `[(x0 - sizeX) >> 3,
  (x1 - 1) >> 3]` at level 1 and those over 4 at level 2, so no search can
  read a stale cluster.
- **The level-2 cache key.** Moho compares children by content. The port
  keys by the children's payload serials (never reused), which is the same
  answer whenever equal windows share a payload, and only a cache miss
  otherwise: a cluster's content is a function of its children's, so a miss
  costs a build, never a different result.
- **Convars.** The engine has no sim convars; the budget is a constant
  (and a setter, for tests).

## Plan

- **4c-1 (this PR): the cluster maps.** `sim/path_clusters` (the payloads,
  both builds, the cache, `ClusterMap` with its dirty bits and background
  work) and `sim/path_tables` (one map a class, the occupation windows).
  The sim makes them with the pathfinding grid, dirties them as ground is
  claimed and released, and spends the background budget each tick. No
  search reads them yet, so nothing a unit does changes. Saved games need
  nothing yet: a load starts every cluster dirty, which no search can see.
- **4c-2a: the search.** `sim/path_search` (Moho's A* with its open heap
  and tie-breaks, the levels a cell expands at, cluster edges built on
  demand from the search's budget, the closest-cell fallback, the army
  queue) and `sim/path_finder` (CAiPathFinder's rules: where to search cell
  by cell, the footprint test skipped for a unit that doesn't fit where it
  stands, the playable area by the footprint's span, goal rects and the
  1.01 octile heuristic). Not yet asked by any unit. Two things it shows:
  - a search spread over many ticks finds exactly what one tick's finds
    (an expansion out of budget is redone whole next tick);
  - near its start a search goes cell by cell only within the start's
    8 x 8 cluster, so a unit can walk out of an obstacle only as far as
    that cluster's edge; past it, the cluster portals must be open.
- **4c-2b: following a path.** `sim/path_walk` (Moho's grid line walk and
  footprint sweeps: a straight step down its middle, a diagonal twice past
  opposite corners) and `sim/path_navigator` (CAiPathNavigator: ask next
  tick, string-pull up to ten cells ahead under 50 off by sweeping the
  footprint, drop the cells behind, ask the way to the next cell on a new
  layer, a stall or a steering refresh, merge the answer in front, give up
  after three failures of three). Tested with a stand-in unit; no unit
  uses it yet. What it shows of Moho:
  - a path's portal jumps are steered at straight, even across a wall; the
    unit's motion refuses the move, its steering asks a refresh, and the
    repath (the 16 x 16 round the unit, cell by cell) finds the way round;
  - a new goal forgets the layer it pathed on, so the first step with a
    path asks once for the way on (one tick waiting);
  - a unit held still 30 ticks stops where it is, as arrived.
- **4c-2c: units use it**, behind a sim switch that is off by default:
  the army queues served each tick, the navigator driving the unit at its
  target (top speed through targets outside the goal; a move into a cell
  it won't fit refused, and a refresh asked), and the saved state that
  makes a loaded game continue exactly: the queue (Moho restarts the
  search in flight on a load; the engine's save/load oracle needs it
  resumed), each navigator and finder, and each map's dirty bits and scan
  cursor (clean clusters rebuilt free on load). Then the switch goes on and
  the gate tests move with it: Moho stops a unit in its goal cell, not on
  the exact point.
- **4c-3: repaths** against mobile units, at level 0.
