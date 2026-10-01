# M224h: The Late Game's p99 Spikes — Findings

**Status:** 2026-09-30. Roadmap Phase H (M224, sim hot paths). Diagnosed, and decided
the same day: option 1, retail's behaviour stays, and the budget is split (see
Decision), then option 4.

## Why

After M224g, the late game's p99 tick was left with what M224g's roadmap entry called
"retail AI's once-a-second burst" at `tick % 10 == 1`. The Phase H budget is 16 ms.

## The benchmark today

This is the pinned late game (`tools/bench.py run --scenario late`: SCMP_009, four
retail AIs, seed 4242, 18,000 ticks), Release, on the stack through #235.

- **It is not the game the stored baseline timed.** At tick 18,000 it has 2,060
  units and 28,117 entities. The baseline (`late-release.json`, recorded before the
  day's fidelity fixes) had 1,081 units and 20,656 entities. The AIs now build about
  twice as much.
- So its 112.3 s of sim can't be compared with the baseline's 51.6 s: `bench.py`
  refuses to (another checksum). Per unit it is similar.

| Ticks | mean | p50 | p90 | p99 | max |
|---|---|---|---|---|---|
| all | 6.2 | 5.7 | 11.3 | 33.5 | 76.9 |
| 17,001–18,000 | 12.1 | 10.5 | 14.4 | 46.3 | 76.9 |

All ten slowest ticks are at `tick % 110 == 1`, 67–77 ms.

## Two spikes

A scratch patch timed each phase of `SimState::tick` and logged every tick over
40 ms:

- **Every 110 ticks (`tick % 110 == 1`): 67–75 ms.** 56–64 ms of it is in the script
  threads.
- **Every 70 ticks: 43–47 ms.** About 35 ms of it is the forced Lua collection at
  the end of the tick. This is M224g's schedule, and its mark is what M224g left.

The rest of those ticks is the ordinary tick: entities 5–8 ms, visibility about 1 ms.

## The 110-tick spike is retail's `BaseMonitorCheck`

A second scratch patch timed every thread resume by its outermost function. The cost
is one thread: `AIBrain.BaseMonitorThread` (`/lua/aibrain.lua:3263`, forked at
`:3135`). It runs `BaseMonitorCheck`, then `WaitSeconds(BaseMonitorTime)`, which
defaults to 11 s. That is 110 ticks, which happen to fall on the `% 10 == 1` ticks.

- **The four AIs' monitors run on the same tick.** They are forked on the same tick
  with the same period, so all four land together.

A scratch `/schook` hook timed `BaseMonitorCheck` and the engine calls in it. It used
a real clock in a scratch build: the sim's `GetSystemTimeSecondsOnlyForProfileUse` is
game time, on purpose. At tick 17,821:

| Army | total | `GetStructureVectors` | `GetThreatAtPosition` | `GetNumUnitsAroundPoint` | monitor points |
|---|---|---|---|---|---|
| 1 | 14.9 ms | 0.18 ms (32 vectors) | 0.20 ms (1,404 calls) | 0 | 4,093 |
| 2 | 13.3 ms | 0.15 ms (32) | 0.08 ms (544) | 0.02 ms (38) | 3,964 |
| 3 | 16.3 ms | 0.16 ms (39) | 0.13 ms (885) | 0.02 ms (63) | 4,234 |
| 4 | 14.4 ms | 0.14 ms (33) | 0.13 ms (910) | 0.02 ms (56) | 4,245 |

**The engine's share is 0.3–0.4 ms of each 13–16 ms.**

The rest is the Lua VM running retail's loops. `BaseMonitorCheck` meant to add a
monitor point only for a structure vector it doesn't have yet. But its `continue`
only continues the inner loop, so every run appends every vector again. The list
grows by about 33 points per AI every 11 seconds, to about 4,100 by 30 game-minutes.

Each run then does all of this:

- compares each vector with every point (vectors × points);
- looks for each point among the vectors (points × vectors);
- reads each point's threat.

That is about 270,000 loop iterations per AI, at roughly 50 ns each. The Lua VM is
built `-O2 -DNDEBUG`; 50 ns an iteration is ordinary for Lua 5.0.

**Moho runs the same Lua, so it has the same growth.** M224g's census had already
found this list among the heap's growth
(`docs/plans/2026-09-29-m224g-gc-pause-design.md`), and left it as Moho's.

## Options

The 16 ms p99 cannot be reached in this scenario while the engine plays retail's AI
as Moho does. Even the late median tick is 10.5 ms, with twice the units the budget
was set against.

1. **Keep it (Moho's behaviour).**
   - Re-record the late baseline from main once the stack merges.
   - Restate the Phase H budget for this scenario, or measure it on a scenario the
     size the budget was set for.
   - Work only on what is the engine's: the collection's mark (option 4), and the
     per-unit costs behind the 10.5 ms median.
2. **Fix retail's bug, as a compatibility patch.** Deduplicate the points in
   `BaseMonitorCheck`, as the `continue` meant. The spike would drop to about 0.4 ms
   per AI.
   - It changes the game: a duplicated point raises its alert once per copy, and
     forks a `BaseMonitorAlertTimeout` thread per alert. Replays and checksums against
     Moho-faithful builds differ.
   - It is a policy call. The project has so far kept retail's bugs when Moho
     shares them.
3. **Stagger the four monitors.** Offset each AI's first `BaseMonitorCheck` by a
   few ticks, so each 13–16 ms lands on its own tick.
   - It also changes the game: when an AI sees an alert.
   - It only divides the spike by the number of AIs; the growth stays.
4. **The collection's mark (35 ms every 70 ticks).** Freezing more of the static
   heap, as M224g froze `__blueprints`, is faithful and measurable. Candidates are
   AI templates and effect templates, if nothing writes to them. Incremental marking
   would need write barriers through all of Lua 5.0: a large change.
5. **A faster VM.** No cheap win was found: the build is optimised and the loop
   runs at ordinary Lua 5.0 speed. Anything more is a large, uncertain project.

**Recommendation:** option 1 now (re-baseline, restate the budget), then option 4.
Option 2 only if a faster late game matters more than matching Moho here.

## Decision (2026-09-30)

Option 1. Retail's scripts are not patched or rescheduled to pass a budget.

The Phase H budget is split:
- **Core:** at about 2,000 units, the median tick under 12 ms and p90 under 16 ms.
- **The engine's:** a p99 of the engine's own, measured apart from the scripts.
- **Retail's script spikes** like this one are tracked but don't fail the budget.

Option 4, freezing more of the static heap, is next on the sim's side.

## Method (to repeat it)

- **Phase timing:** a scratch patch in `SimState::tick` with `steady_clock` marks
  after each step, logging ticks over 40 ms.
- **Thread timing:** a scratch patch in `ThreadManager::resume_all` timing each
  `lua_resume`, keyed by the coroutine's outermost Lua function and its fork site,
  split into burst and other ticks, from tick 12,000.
- **Lua detail:** a `/schook/lua/aibrain.lua` hook wrapping `AIBrain.BaseMonitorCheck`.
  It shadows `GetStructureVectors`, `GetThreatAtPosition` and `GetNumUnitsAroundPoint`
  on the brain for the call, with `GetSystemTimeSecondsOnlyForProfileUse` made a real
  clock in a scratch build. The hook is mounted by a custom `--init` that puts a
  scratch folder first on the path, with `hook = { '/schook' }`.

None of these is committed; each is a few lines to redo.

## Option 4, tried (2026-09-30): there is no static heap left worth freezing

An instrumented collector (scratch, not committed) took a census at the late game's
collections. It counted the live tables still marked, by shape and holder, and the
mark's work.

At collection 250 (about tick 17,500) the mark takes 31-34 ms. It visits:
- 330,000-365,000 tables, with 2.4 million array and hash slots;
- 5,800 threads, with 295,000 stack slots.

The frozen blueprints (203,000 tables) are already out of it.

- **What it marks is live game state:**
  - each unit's empty event-callback lists and effect bags (66,000 empty tables);
  - the props' objects (19,000);
  - trash bags (36,000, growing);
  - retail AI's platoon lists and monitor points;
  - upvalues (about 110,000) and coroutines.
  None of it can be frozen: it changes, and it dies with its units.
- **Static data reachable from modules and globals** totals 42,000 tables. 38,000 of
  them are `/lua/basetemplates.lua`'s AI base layouts.
- **Freezing the base templates changed nothing:** in a trial, the mean mark over the
  last 84 collections of the same game was 34.11 ms without and 34.10 ms with. They
  were allocated together at load, so marking them was already cheap; the cost is the
  live objects scattered through the heap.
- **The largest single table** (131,072 nodes) is retail's `repr.lua` `global_names`.
  It maps every table within two levels of `_G` to its name, and was built when
  `globalInit.lua` re-ran `repr.lua` after `__blueprints` existed. Moho does the same:
  `Sim` exports `__blueprints` (`ExportToLuaState`) before `simInit.lua`. Some of its
  keys have metatables, so freezing it would only make it a frozen root, traversed as
  before.

**Conclusion:** the mark is set by live state. Only an incremental or generational
collector would shorten it, and that means write barriers throughout Lua 5.0's VM
(table writes, upvalues, stacks) while keeping the game deterministic: a large change,
not to start without a decision. Freezing more is not worth a change.

