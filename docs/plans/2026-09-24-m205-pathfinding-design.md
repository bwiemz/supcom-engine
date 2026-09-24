# M205 — Pathfinding

Status: design, 2026-09-24. The fifth milestone of Phase E (gameplay fidelity). Since units drive (M203) and platoons follow their routes (M204), how fast and how well paths are found sets how an army moves.

## Why

Pathfinding had become a fifth of the sim's time. In the four-AI benchmark (SCMP_009, 6,000 ticks) A* took about 21% of the profiler's samples. Every search is capped at eight a tick, so a large army waits its turn for paths.

## What the survey found

These findings come from counting a benchmark game's searches and profiling it with stack sampling.

- **Searches:** 7,867 in 6,000 ticks, expanding 33.2 million cells, about 4,200 each. Only 9 hit the 50,000-cell limit, so searches for unreachable goals are rare and not the cost.
- **Where the time went:** A* itself took 68 of the 70 pathfinding samples; path smoothing and line of sight barely register.
- **Per cell:** each expanded cell tested its 8 neighbours (and a diagonal's two corners) through `is_passable_for`, which compared the layer's name on every call.
- **Per search:** each search cleared three grid-sized buffers, about 2 MB on a 512 × 512 grid.
- **The heuristic:** the octile distance is exact on open ground. There many cells share one cost, and A* expanded the whole plateau of them before reaching the goal.

## Slices

### M205a: search cost

- **Resolve the mover once:** a search works out the mover's passability class (air, land, water with its draft, amphibious) once and tests cells by index.
- **Stamp the buffers:** search buffers carry a stamp per search instead of being cleared.
- **Weight the heuristic by 1.001:** among cells of equal cost A* follows the ones nearer the goal, and a path stays within 0.1% of the shortest.
- **Proof:**
  - with the first two alone, the benchmark game plays exactly as before;
  - a unit test holds a search across open ground to the cells it needs.

**What building M205a established:**

- **Tie-breaking was the cost, not the work per cell.** Resolving the mover
  and stamping the buffers saved about 2% (25.2 s to 24.7 s), with every
  path unchanged. Weighting the heuristic by 1.001 cut the cells expanded
  from 33.2 million to 3.7 million over the game, and the benchmark fell to
  about 19.5 s. A weight of 1.01 saves hardly any more.
- **The effort test needs a plateau.** A path straight along the diagonal
  has one cheapest route, and A* expands only its cells with or without the
  weight. A path off the diagonal (61 cells expanded against 799 without the
  weight) shows the difference.

### M205b: queued throttling (deferred)

- **The plan:** requests over the tick's budget wait in order, rather than every waiting unit retrying each tick. Retrying favours whichever units update first. Chasing units re-path when their target moves, and would take their place in the queue like any other request.
- **Measured, not needed yet:** with searches 9 times cheaper, only 29 of the benchmark's 9,172 requests (0.3%) found the tick's budget spent. This waits until larger games, or more AIs, show waiting that matters.

### M205c: hierarchical search

- **Regions and portals:** long paths are found over a coarse graph of regions and portals, and refined only near the unit.
- **`CanPathTo`** answers from the same graph (it answers from grid connectivity since M185).
- **The seabed** becomes a layer for amphibious units.

## Risks

- **Paths change a little.** A weighted heuristic may pick a different path of about the same length, so games differ from before. The long AI games and the cross-OS replay check them.
