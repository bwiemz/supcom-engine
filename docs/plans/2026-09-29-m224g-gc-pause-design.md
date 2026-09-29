# M224g: The Sim's GC Pause — Design

**Status:** 2026-09-29. Roadmap Phase H (M224, sim hot paths).

## Why

The M223 benchmark put the late game's p99 tick at 40 ms (72 ms in its last 1,000
ticks), against Phase H's 16 ms budget. The ten slowest ticks were all multiples of
70: the sim's Lua full collection, which runs on Moho's schedule (`Sim::AdvanceBeat`,
M224e). Lua 5.0's collector stops the world: it marks everything reachable, then
sweeps every object.

## What a collection spends (measured)

A temporary instrumented `lgc.c` timed the phases and took a census of the live
tables, grouped by metatable, or by shape for plain tables. It also walked holders
back to their roots. At the late game's last collection:

| Phase | ms |
|---|---|
| mark | 32.9 |
| sweep, objects (`rootgc`) | 30.9 |
| sweep, strings | 5.0 |
| weak tables | 1.7 |
| finalizers | 0 |
| **total** | **70.5** |

- **Live tables:** 292,000 at tick ~4,500 and 459,000 at 18,000. About 235,000 are
  static data: the blueprints (`__blueprints`, including the effect blueprints' curves:
  50,785 `{XRange, Keys}`; 57,654 `{x, y, z}`; about 113,000 small arrays). So static
  data is 80% of the early heap and half of the late one, and every collection marks and
  sweeps it again.
- **Where the growth comes from:** retail's AI scripts. `BaseMonitor.BaseMonitorPoints`
  grows without bound: `BaseMonitorCheck`'s `continue` never skips a point it already
  has, so every check inserts every structure point again. Builders' `InstanceCount`
  slots grow as bases are added. Coroutines grow from 167 to 8,000 and trash bags to
  26,000. Moho runs the same scripts, so none of that is the engine's to change.

## What changes

Both parts keep what scripts can see: the same objects die on the same ticks, weak
entries clear at the collection, and finalizers run there.

### 1. A lazy sweep

- A collection marks as before, then sweeps userdata and strings at once. Strings must
  go at once: interning could hand the program a string that is still garbage.
- It leaves `rootgc` for `luaC_sweepstep`. It detaches the marked list, and new
  objects go to a fresh `rootgc` that this sweep never visits. When the sweep ends, the
  survivors rejoin at its head. Garbage is unreachable, so freeing it later changes
  nothing a script can observe.
- **Slices by work:** `lua_sweepstep(L, work)` charges 1 per live object and 8 per
  freed one. The newest objects, swept first, are mostly garbage, and `free()` is most
  of a sweep's cost. The sim sweeps 80,000 units at the start of each tick, which frees
  at most 10,000 objects a tick. A sweep takes a few dozen ticks, well inside the
  70-tick period.
- **Ending:** a collection ends any sweep still under way first; so does `lua_close`,
  and so does turning lazy sweeping off.
- **The automatic trigger:** Lua's threshold is held until the sweep ends, then set as
  `checkSizes` would have set it. The heap never doubles within a sweep's few ticks.
- **Loading is unchanged:** lazy sweeping starts at the first tick, since loading runs
  no ticks to advance a sweep.

### 2. Frozen static data

- **`lua_freeze(L, idx)`:** the plain tables reachable from a table (no metatable, so
  not weak either) become fixed. Their mark bit is permanently on, so the mark passes
  them by and weak tables keep them. They also move off `rootgc` to `frozengc`, which
  only `lua_close` sweeps. Their strings are fixed too.
- **Frozen roots:** a frozen table that holds anything else (a function, a userdata, a
  table with a metatable) is a frozen root. The mark traverses it every collection, so
  what it holds lives.
- **The write barrier:** `luaH_set`, `luaH_setnum` (every table store goes through
  them) and `lua_setmetatable` make a frozen table that is written to a frozen root.
  What it's given is marked through it from then on. A frozen table never dies: a
  blueprint subtable a script replaces stays in memory. That's harmless for static data.
- **What's frozen:** the sim freezes `__blueprints` at its first tick, after loading,
  mods' `ModBlueprints` and the scenario's boot.

## Measured (Release, the late benchmark: four AIs, 18,000 ticks)

| | before | lazy sweep | + frozen blueprints |
|---|---|---|---|
| p99 tick | 40.1 ms | 30.3 ms | **18.4 ms** |
| p99, ticks 1–1,000 | 17.8 ms | 14.0 ms | 4.4 ms |
| p99, ticks 5,001–6,000 | 31.1 ms | 22.5 ms | 8.8 ms |
| p99, ticks 17,001–18,000 | 72.3 ms | 45.1 ms | 29.9 ms |
| sim time | 51.6 s | 52.2 s | **45.1 s** |
| peak memory | 365 MB | 365 MB | 365 MB |
| game (checksum at the end) | `1be9b3ee` | `1be9b3ee` | `1be9b3ee` |

- **The same game:** freeing garbage later, and never marking the blueprints, changed
  nothing the sim does.
- **What now sets the slowest ticks:** the collection tick is gone from the top ten.
  All ten now fall on `tick % 10 == 1` (47–54 ms): retail's AI threads that wait whole
  seconds all wake together. That burst, not the collector, is what's left of the p99,
  and it's the next target.

## Tests

- **Unit (`[lua_gc]`):**
  - a lazy sweep frees garbage over its slices and keeps live objects and objects made
    meanwhile;
  - a collection during a sweep ends it first, and turning lazy sweeping off ends it
    too;
  - `lua_close` in the middle of a sweep (under ASan/LSan);
  - weak tables clear at the collection, before any slice;
  - frozen data survives collections intact, including a cycle;
  - every write path into a frozen table keeps what it stores: field set, `rawset`,
    integer keys, `table.insert`, a string, a function;
  - non-freezable children and a later `setmetatable` stay alive;
  - weak tables keep frozen keys and values;
  - freezing a non-plain table, or twice, is harmless;
  - `lua_close` frees frozen tables and the roots list.
- **The benchmark:** the same checksum at every stage (above).
- **The unit suite, the data suite, and an ASan build** running the GC tests and an AI
  game.
