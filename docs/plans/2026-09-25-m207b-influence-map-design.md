# M207b — Influence maps

Status: design, 2026-09-25. The second slice of M207 (AI query fidelity).

## Why

The AI's threat queries count every enemy unit. `GetThreatAtPosition` and its siblings read live units through fog, so the AI knows what it has never seen and forgets nothing it has. Moho's queries read the army's **influence map**: a grid of threat kept from the blips its intel has reported. It fades once a mobile unit is out of sight, and a structure stays on it until the army sees that the structure is gone. Retail's AI is written against that map. It scouts to fill it in, and treats a quiet cell as uncertain rather than empty.

## What the decompile shows

This comes from faf-re (`CInfluenceMap`, `CArmyImpl`, `CAiReconDBImpl` and `CAiBrain`). Addresses are in the research notes this design was written from.

**The grid.** Each army has one influence map, created with the army.
- The cell size is `max(32, max(map width, map height) / 16)`, so maps of 10 km and up are 16 cells across. A 5 km map is 8 cells of 32.
- Each cell keeps three things:
  - one entry per blip it holds;
  - one set of threat lanes per army, rebuilt at every update;
  - a set of script-assigned lanes (`AssignThreatAtPosition`), each with a decay rate.

**The feed.** Every `army count` ticks, each army's recon pass feeds its map. It feeds every unit that is not the army's own, not being destroyed, in `VISIBLETORECON`, and either:
- detected by any of the army's senses;
- allied (always counted as detected); or
- a structure the army has had in line of sight at some point.

Each unit enters as a blip, at its position, with its real blueprint.

**An entry.** A fresh or re-seen blip has strength 1 for 10 updates. After that its strength falls by 0.02 per update if it is mobile. A structure's strength never falls.
- A blip that moves cell is removed and inserted again.
- An entry is removed outright only when the army sees the spot of a dead structure, or the structure was allied. A dead or lost mobile unit fades out instead.

**The update.** Every 30 ticks, army `i` updates its map on ticks where `tick % 30 == i`.

1. Entries decay, and those at 0 go.
2. For an entry whose source is neither the army nor an ally, and whose unit still exists:
   - it takes the unit's current layer;
   - it becomes **detailed**, for good, once the army has the unit in omni or has ever had it in line of sight.
3. Each entry adds to its source army's lanes. With `s` its strength, the blueprint gives
   - `aa = Defense.AirThreatLevel·s`, `su = SurfaceThreatLevel·s`, `sb = SubThreatLevel·s` and `ec = EconomyThreatLevel·s`;
   - `total = aa + su + sb + ec`, which goes into **Overall**.
   - A structure adds `total` to **Structures**, and to **StructuresNotMex** unless it is a mass extractor.
   - A mobile unit adds `total` to **Air** if it can fly, else to **Land** or **Naval** by its layer (Land; Water, Seabed or Sub).
   - A detailed entry adds `total` to **Experimental** or **Commander** by category, and `aa`, `su`, `sb` and `ec` to **AntiAir**, **AntiSurface**, **AntiSub** and **Economy**.
   - An undetailed entry adds `total` to **Unknown**.

**A cell's threat.** For a type and an army:
- It is the cell's script-assigned lane for that type (none for `OverallNotAssigned`), plus that army's lane.
- With no army given (-1), it adds the lanes of every army the map holds, allies included.

**Script-assigned threat.** `AssignThreatAtPosition(pos, value, rate, type)` adds `value` to the cell's lane for the type.
- `Overall` and `Unknown` both write the Unknown lane.
- The lane then falls by `lane × rate` per update, so it is gone after `1/rate` updates. The rate is clamped to [0, 1] and defaults to 0.01.

**The queries.** `rings` is a radius in cells around the position's cell.
- `GetThreatAtPosition(pos, rings, onMap, type, army)` sums the cells of a square of side `2·rings + 1`. The fourth argument keeps the square inside the playable area; it is not a visibility check.
- `GetThreatsAroundPosition` returns `{x, z, threat}` rows for every cell in that square with threat above 0, highest first. `x` and `z` are the cell's centre.
- `GetThreatBetweenPositions(a, b, onMap, type, army)` walks the cells from one to the other (Bresenham) and returns the sum of each cell's threat.
- `GetHighestThreatPosition(rings, onMap, type, army)` looks at every cell, each summed over its square when `rings > 0`. It starts from a best value of -200, and ties go to the cell nearer the army's start. It returns the cell's centre and the threat.
- Threat types are named without their `THREATTYPE_` prefix. They are case-sensitive, and an unknown name is an error. The army is 1-based.

## Where the decompile is doubtful, and what we do

1. **Detail.** Read literally, `Update` checks the source army's recon on its own unit, which would never mark an enemy as detailed. Retail's AI plainly sees `AntiSurface` threat, so we use the map owner's recon: omni now, or line of sight ever.
2. **The structure lanes.** Moho's field names and its per-type lookup disagree. We go by meaning: `Structures` counts every structure, and `StructuresNotMex` leaves out mass extractors.
3. **Artillery.** Moho looks up the category `"ARTILLERY, STRATEGIC"` by its exact name. No category has that name, so the lookup misses and the Artillery lane stays 0. We keep it at 0, matching the binary.
4. **Allies.** Allied units are fed and count toward Overall, Unknown, Structures and Air. They are never detailed and have no layer, so they add nothing to Land, Naval or the Anti lanes. We do the same, and the test says so.

## Our side

- **Per-army intel** already exists. `SimState::update_visibility` paints each army's vision, radar, sonar and omni every tick and merges allies. The OnIntelChange pass works out, per unit and army, which senses see it, with stealth and cloaking applied.
- **Units already carry** their blueprint's four threat levels (`air_threat` and so on) and their motion type (`is_mobile`).
- **To add:**
  - a sticky "ever in line of sight" bit per unit and army, for detail and for remembered structures;
  - entries that outlive their units. An entry copies what it needs from the unit (its threat levels, whether it is mobile, flies, or is a mass extractor, commander or experimental), since a blueprint outlives its units but ours are read from the unit.

## The slice

- **`sim::InfluenceMap`** (`src/sim/influence_map.{hpp,cpp}`) is free of Lua: the grid, entries, update and queries.
  - Cells hold entries in a `std::map` keyed by entity id, and a blip's cell is found through another ordered map. Iteration order is fixed, so every peer computes the same.
- **`ArmyBrain`** owns its army's map. It is created the first time the army is fed, once the terrain is known.
- **In `SimState::tick`:**
  - after `update_visibility`, the army whose turn it is (`tick % army count`) is fed;
  - the map due to update (`tick % 30 == army`) updates at the start of the tick, as Moho's army `OnTick` does before scripts run.
- **The brain's bindings** answer from the map:
  - `GetThreatAtPosition`, `GetThreatsAroundPosition`, `GetThreatBetweenPositions` and `GetHighestThreatPosition` query it;
  - `AssignThreatAtPosition` stops being a no-op;
  - `FindPlaceToBuild` honours `optIgnoreThreatOver`, rejecting a site whose cell has AntiSurface threat at or above it.
- **The checksum:** the armies domain mixes each map's entries (unit id and strength), as Moho mixes strengths into its own. Each cell's threat is worked out from its entries at every update.

## Proof

- **Unit tests** on the map alone: the cell size and grid, strength over a timeline (held for 10 updates, then falling 0.02 per update), a structure staying, cell moves, lanes by kind (structure, mass extractor, air, land, naval, detailed or not), allies, script-assigned threat and its decay, and each query's square, playable clipping, ordering and Bresenham sum.
- **A gated runner test** with retail units. An army sees an enemy tank and has its threat. The tank leaves sight and its threat fades on schedule. A structure it has seen stays. A structure it never saw is absent. Queries answer in cells.
- **The existing threat test** is changed to give its brain intel of the unit it asks about. Its old premise, that an army knows an enemy it has never seen, is what this slice removes.

## Risks

- **The AI will play differently.** It will be blind where it hasn't scouted, as retail's is. Long AI games will show whether that exposes other gaps, for example scouting that doesn't run or threat checks that stall builders.
- **Cadence.** Feeding every `army count` ticks and updating every 30 ticks makes answers lag, as in Moho. Tests must wait for an update and not read the map straight after a change.
