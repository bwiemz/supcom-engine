# M223: The Sim Benchmark — Design

**Status:** 2026-09-29. Roadmap Phase H (performance and scale).

## Why

Phase H's budget is a sim tick p99 of 16 ms or less with 1,500 units. M224's six passes
(category expressions, the unit grid, entity lookup, emitter lifetimes, the GC schedule,
placement queries) took the 18,000-tick four-AI game from 450 s to about 69 s. Every one
of those numbers was measured by hand: a `time.monotonic()` wrapper, log timestamps, two
binaries side by side. Nothing holds the gains. A fidelity change that makes the AI's
queries slower, or leaks script objects, shows up only as a slower game, not as a
failing test (see the sim-profiling notes: "perf regressions from fidelity work show up
as a slower sim, not as errors").

## What it is

- **`--bench <report.json>`** (headless `--ticks` and `--ai-skirmish` runs). The run's
  sim ticks are timed one by one (`App::tick_headless`). It reads a steady clock around
  `SimState::tick()` and nothing else, so the game is the one it would be without it.
  The report holds:
  - the load time (from the run's start to the first tick);
  - the ticks' figures: total, mean, p50/p90/p99 by nearest rank, the slowest and
    which tick it was, overall and for each 1,000 ticks, so a late-game cost shows
    where it starts;
  - the game at the end: tick, sync checksum, entity and unit counts;
  - peak memory (`getrusage`, or the peak working set on Windows);
  - the version, build id, build type and OS.
- **`tools/bench.py`:**
  - `run` plays a pinned scenario, keeping the fastest of `--repeat N`, and checks
    the runs all played one game;
  - `compare` holds a report to a baseline: sim time, the mean and p99 tick, and peak
    memory, each within `--tolerance` (15%);
  - `check` does both against `$OSC_GOLDEN_DIR/bench/<scenario>-<build type>.json`,
    recording the baseline when there is none (or with `--update`). A failing run
    leaves its report beside the baseline (`.new.json`) to look at or adopt.
- **The scenarios:** four AIs on Seton's Clutch, seed 4242. `early` is 6,000 ticks (10
  game minutes); `late` is 18,000 ticks (30 minutes), where the AI's queries and Lua's
  collections grow.
- **CTest:**
  - `bench.early` and `bench.late`, label `bench`, `RUN_SERIAL`, skipped (77) without
    game data or `$OSC_GOLDEN_DIR`;
  - `arch.bench_self_test`, data-free, on every CI platform: the comparison rules.

## Decisions

- **Only the same game compares.** When the checksum at the end differs, the timings
  describe different games (the sim-profiling notes: "a different game invalidates the
  timing comparison"). So `compare` refuses ("not comparable", exit 2) and says what
  to do: time the change against the previous build's binary, or record a new
  baseline. A change that alters the sim has to make that choice on purpose.
- **Baselines are per machine and per build type**, kept outside the repository with
  the goldens. Absolute timings mean nothing across machines, and a Debug build is
  several times slower.
- **Not in CI.** The roadmap asked for budgets asserted in CI. But CI has no game data,
  and a data-free sim (the synthetic game in `test_cross_platform.cpp`) exercises
  movement only, not the Lua-heavy AI where the time goes. Shared runners' timing
  noise would also flake any tight budget. CI runs the comparison rules
  (`arch.bench_self_test`); the budgets run where the data is. That matches how the
  data tests and goldens already work.
- **Load time is reported, not budgeted:** it is dominated by disk caching.

## Tests

- Unit: `tick_stats` (nearest-rank percentiles, the slowest tick, one tick, none).
- `bench.py --self-test`: within tolerance; each metric over it; faster; another
  game; another build type.
- By hand: baselines recorded from a Release build, then each scenario run again
  against its baseline, to see the run-to-run noise against the tolerance.

## First measurements (2026-09-29, Release, this development machine)

| Scenario | Load | Sim time | Mean tick | p50 | p90 | p99 | Slowest | Peak memory | End |
|---|---|---|---|---|---|---|---|---|---|
| early (6,000 ticks) | 8.8 s | 6.0 s | 1.0 ms | 0.6 ms | 1.1 ms | 21.9 ms | 60.2 ms (tick 3,241) | 272 MB | 281 units, 9,308 entities |
| late (18,000 ticks) | 8.7 s | 51.6 s | 2.9 ms | 1.7 ms | 4.3 ms | 40.1 ms | 78.9 ms (tick 17,360) | 365 MB | 1,081 units, 20,656 entities |

- A second run of each against its baseline: within 0.4–3.8% on every metric, the
  same game each time. The 15% tolerance leaves room for a busy desktop, and still
  catches a real regression.
- **What breaks the p99 budget (16 ms):** in the late game's last 1,000 ticks the
  median tick is 4.6 ms and p90 5.8 ms, but p99 is 72 ms, and p99 grows window by
  window with the live heap (18 ms at the start). The slow 1% are the Lua full
  collections every 70 ticks (1.4% of ticks; M224e, Moho's `Sim::AdvanceBeat`
  period). Measured, not assumed: the report's ten slowest ticks (72–79 ms) all fall
  on multiples of 70. Each collection costs more as the heap grows, since Lua 5.0
  has no incremental collector. That pause, not the ticks' work, is the next Phase H target: a smaller
  live heap, or an incremental collector that keeps the game deterministic.
