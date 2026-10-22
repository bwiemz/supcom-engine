# Measuring engine performance

The simulation normally runs on its own thread. While it is busy, frames
interpolate published world snapshots and reuse prepared UI and custom
world geometry. UI callbacks, render asset scripts and live-sim queries run
while the simulation is settled. `--no-sim-thread` selects serial execution
for comparison. `--no-workers` disables the pool used for world capture.
Visibility retains its serial Lua callback order.

## Repeatable runs

Use the same build, map, data, seed, factions and orders for each comparison.
Check checksums before attributing a timing difference to an optimization.
Random lobby factions are chosen before sim initialization, so choose fixed
factions as well as fixing the seed. Run comparisons back to back under a
similar machine load.

`--fast-match <ticks>` runs an offscreen, unpaced game to a tick limit.
`--match-telemetry <file>` records tick timings and army statistics as JSONL;
its final record includes mean and worst tick times. This exercises the
windowed session path, including world capture. `tools/bench.py` also offers
headless sim benchmarks, which do not include rendering or world capture.

`--play-for <seconds> --play-rate <rate>` instead measures paced frames at a
requested game speed. `--play-until-tick <tick>` adds a tick limit. The frame
summary reports slow frames and changes in drawn pace. This is useful for
checking whether long sim ticks interrupt rendering at accelerated speeds.

## Profiles

`--profile` prints totals by `PROFILE_ZONE`: inclusive time, self time
(excluding nested zones), calls and worst frames. It runs the simulation on
the main thread so its zones share one owner. `--profile-frames` profiles
frame work while leaving simulation threading enabled. Engine functions
called from Lua are timed as profile zones.

The Lua instruction sampler reports functions by source and definition line.
Set `OSC_LUA_PROFILE_LINES=1` to group samples by the executing source line.
Samples are taken every 1,000 instructions; they measure instruction share,
not wall time. Slow Lua resumes are logged with their resume and fork sites.

## Measuring a saved moment

Save repeatable late-game scenes with:

```text
--fast-match <last-tick> --save <dir>/match-{tick}.sav --save-at 12000,24000,36000
```

Then measure a saved scene with:

```text
--load <save> --fast-match <last-tick> --profile --bench <report.json> --bench-scenario <name>
```

Use the same `--user-dir` for both runs. Snapshots are authenticated with that
folder's key; an untrusted snapshot instead catches up from the recording.
A snapshot format change requires recreating saved moments. Benchmark JSON
includes tick timings, unit counts, final checksum, profile zones and Lua
samples.

## Match records and marks

Drawn games add a performance sample every ten seconds: frame mean, p95 and
worst time, frames over 33 ms, held frames, requested and achieved speed,
sim tick time and world size. UI scripts can call
`MarkPerformanceMoment('description')` to add a `player_mark` event. This port
does not install a keyboard shortcut or a match-history graph.

## Port provenance

These features originate in [WOPC's engine fork](https://gitlab.com/with-our-powers-combined/opensupcom),
including its profiling and snapshot work (!55), simulation thread (!60,
!69), drawing clock and CPU UI scaling (!71, !72), capture workers (!74),
line profiling (!75), patrol queries (!76) and saved-moment benchmarks (!78).
The fork's published benchmark numbers describe its own builds and AI hooks;
they are not measurements of this port.

Current `main` already skips idle props in the entity walk. This port also
reuses unchanged plain-prop snapshot records and interns snapshot strings.
Projectiles still rebuild their conditional record fields. Capture workers
write disjoint output records and join before publication; recon remains
serial because its Lua intel callbacks can observe earlier updates.
