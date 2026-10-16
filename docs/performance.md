# Performance: how to measure, and where the time goes

The game a player plays is the windowed loop: each frame advances the sim on
the main thread, then draws. A tick that takes longer than a frame (16.7 ms
at 60 fps) holds that frame back, and the player sees a stutter. So the sim's
tick time is what to cut, and its spikes matter as much as its mean.

## Measuring

**A WOPC AI match, timed.** `--fast-match <ticks>` plays the windowed loop
offscreen and unpaced, a sim tick a frame (`--match-telemetry` writes each
tick's wall time; its `end` record has the mean and the worst). Add
`--profile` and the run ends with the profiler's totals:

```
=== Profile totals: 6000 frames, 125.3 s, mean 20.88 ms, worst 190.8 ms, 41 over 33 ms ===
  Zone              Total s      %      Calls  ms/frame   Worst ms at frame   slow
  Sim::threads        30.51  24.4%       6000     5.085     186.15     4122      9
```

Each zone (`PROFILE_ZONE`, `src/core/profiler.hpp`): its time over the run,
its share of all frame time (nested zones count in their parents too), its
worst frame and which frame that was, and how many frames it alone took over
33 ms (a hitch at 60 fps). The `Frame::` zones split the windowed frame; the
`Sim::` zones split the tick (`SimState::tick`). Under `--profile` a Lua
thread whose resume takes over 30 ms is logged too (`Slow Lua thread:`),
with where it stopped and where it was forked.

A match the way CI plays it: `wopc/ci/match_config.lua` (eight AIs on
SCMP_001), launched as `wopc/ci/match_test.py` does. Runs vary by 10-20%
with what else the machine does (CI's jobs run on the same PC): compare
builds back to back, and use the zones' shares as well as their times.

**Where in the code: the sampler.** `tools/sample_profile.py <pid>` samples
the engine's busiest thread (the main thread) a few hundred times a second
and names the functions on its stack from the PDB, as a share of the
samples: self (the function it was in) and total (anywhere on the stack).
It needs symbols, which a Release build doesn't make by default:

```
cmake -S . -B build-release "-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Zi" \
      "-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Zi" \
      "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/DEBUG /OPT:REF /OPT:ICF"
```

(the same optimization as Release; only the PDB is added). Start a match,
let it load (~20 s), then sample it for a few minutes. `--lines NAME` splits
the samples in functions naming NAME by source line.

**The benchmarks** (`tools/bench.py`, *Benchmarks* in `current-state.md`)
time headless ticks: the sim alone, without the world snapshot below, which
only the windowed loop takes.

## Where the time goes (2026-10-16)

An eight-AI WOPC match on SCMP_001, ten game-minutes (6,000 ticks), fluid
movement, seed 101: 12,700 entities at the start (most of them props), 24,000
at the end. The tick is 95% of the frame; drawing is under 1% in a fast
match, which draws one frame in fifty.

| Part of the tick | Before | After the snapshot change |
| --- | --- | --- |
| The tick, mean | 25.5 ms | 20.9 ms |
| World snapshot (`Sim::observer`, `capture_world`) | 8.9 ms | 4.0 ms |
| Lua threads (the AIs' and units' scripts) | 5.1 ms | 5.1 ms |
| Entities (`update_entities`: units, projectiles, props) | 5.5 ms | 4.8 ms |
| Visibility | 1.5 ms | 1.4 ms |
| Replay checksum (`checksum_parts`, every tick) | ~2.4 ms (sampled) | the same |

**The world snapshot** (`capture_world`, `world_snapshot.hpp`) copies every
entity into a record for the renderer each tick. It freed and rebuilt them,
five `std::string`s each; its records are now plain memory (the names
interned, `core/interned_string.hpp`) rewritten in place. What remains is
reading 24,000 entities spread over the heap (cache misses), most of them
props that haven't changed.

**Stutters.** Single ticks of 100-200 ms: a Lua thread's resume (the worst
seen, 186 ms) and the Lua collection every 70 ticks (stop-the-world, up to
130 ms). The slow-thread lines name the scripts.

## Ideas not yet done

- Props: the snapshot and `update_entities` visit every prop every tick;
  a prop that hasn't moved, fallen or changed could keep its record and be
  skipped.
- The replay checksum walks every entity every tick again.
- The sim on its own thread: the snapshot already separates what the
  renderer reads from the live sim, so a slow tick would no longer hold a
  frame back.
- Lua: `table.sort` from AI scripts (4.5%), `CategoryMatcher` compiled
  again on each call, `GetBlueprintStat` building a string set per
  blueprint.
