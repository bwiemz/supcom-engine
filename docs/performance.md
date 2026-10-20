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

**A drawn game, across the whole match.** Every drawn game's match record
(a player's, or `--match-telemetry`) has a `perf` record each ten seconds:
frames (mean, slowest 5%, worst, over 33 ms), frames the drawn world held,
the drawn pace's change, a tick's own run time, speed asked and ticks
played, units and entities; and a `player_mark` event where the player
pressed Ctrl-M (WOPC's key: a lag felt). WOPC's match history graphs them
(Performance), and its `scripts/telemetry/perf_report.py` reads a match a
stretch to a line. `--play-for <s> --play-rate <n> --play-until-tick <t>`
plays the eight-AI match drawn at a speed to a tick or a win.

**Which scripts: the Lua profile.** Also under `--profile`, the end of the
run lists the Lua functions that ran the most instructions (a sample every
1,000, `src/sim/lua_profile.cpp`), by source and the line each starts on:

```
=== Lua profile: 1370956 samples, one per 1000 instructions ===
   29.9%    409791  /lua/ai/gridpresence.lua:129
    8.8%    121296  /lua/ai/gridrecon.lua:68
```

**The same game twice.** A slot whose `Faction` is 5 (random) draws its
faction afresh each run, not from `--seed`: CI's match config has five.
Fix the factions before comparing two builds, and compare their telemetry
(`--match-telemetry`) record by record, wall times left out: a change that
should only be faster must play the same game. Two builds compared at once,
side by side (two ports), see the same load from the rest of the machine.

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

With all three changes (the snapshot, idle props, WOPC's GridRecon hook)
against fcf7faeb, run side by side on the same match with fixed factions:
36.0 ms a tick against 25.6 ms (-29%), every telemetry record the same.

**The AIs' scripts.** In the Lua profile two of FAF's AI grids ran 70% of
all Lua instructions: GridRecon's five cell sorts every 11 ticks per AI
(about 40%, read only by its debug view: WOPC sorts them only while
debugging, `gamedata/wopc_patches/schook/lua/ai/gridrecon.lua`), and
GridPresence's flood fill every 30 ticks per AI (`gridpresence.lua:129`, 30%,
53% once GridRecon's sorts went). GridPresence draws from the sim's random
numbers, so a faster version must draw the same numbers in the same order.

**The world snapshot** (`capture_world`, `world_snapshot.hpp`) copies every
entity into a record for the renderer each tick. It freed and rebuilt them,
five `std::string`s each; its records are now plain memory (the names
interned, `core/interned_string.hpp`) rewritten in place. What remains is
reading 24,000 entities spread over the heap (cache misses), most of them
props that haven't changed.

**Stutters.** Single ticks of 100-200 ms: a Lua thread's resume (the worst
seen, 186 ms) and the Lua collection every 70 ticks (stop-the-world, up to
130 ms). The slow-thread lines name the scripts.

## Stutters in play: frames, and the sim's own thread

What a player sees is frames, not ticks: at 60 Hz a frame over ~20 ms is a
dropped frame. **`--play-for <seconds>`** plays a game in real time,
offscreen, its frames paced as a 60 Hz display paces them (the rest of each
frame waited out, as a present waits), and logs at the end:

```
Frames in the game: 17943, mean 16.7 ms, over 20 ms: 44, over 33 ms: 16, over 50 ms: 4, worst 188.9 ms (sim thread: on)
```

Since OE-134 the sim ticks on its own thread while the frame draws
(`docs/plans/2026-10-16-sim-thread-design.md`); `--no-sim-thread` puts it
back on the main thread for comparison. Five real-time minutes of the
eight-AI match, the two run side by side:

| Sim thread | Frames over 20 ms | over 33 ms | over 50 ms |
| --- | --- | --- | --- |
| off | 220 | 51 | 11 |
| on | 44 | 16 | 4 |

(A second pair, with CI building on the same PC: over 33 ms 48 against 15,
over 50 ms 7 against 4; single dropped frames rose for both with the load.)

The tick still has to finish within about a frame and a half to drop
nothing: the Lua collection's pause (OE-135) and the longest AI ticks still
show as single hitches.

**A game sped up past its sim's pace** (Ryan, 2026-10-17: the frame rate
fell when he sped the game up). A frame used to wait for the ticks the last
one started; sped up, a frame starts up to 8 of them, and late in a match
that held each frame for a fifth of a second. Now a frame that finds them
still running only draws (`Window::draw_while_ticking`): the window's input,
the camera and the world from the published ticks, the game's time owed to
the next ticks (at most 8), and no UI script or live-sim read. The game
slows to what its sim keeps up with; the frames don't. `--play-rate <n>`
plays `--play-for` at game speed n. The eight-AI match, two minutes at 10x
speed (rate 10), the same build:

| Frame | Mean | Over 33 ms | Over 50 ms | Worst | Ticks played |
| --- | --- | --- | --- | --- | --- |
| waits for the ticks | 72.2 ms | 1,131 | 1,011 | 711 ms | 5,349 |
| draws while they run | 16.7 ms | 1 | 1 | 139 ms | 4,921 |

**Rules the sim's thread brings.** While a tick runs there, nothing on the
main thread reads the live sim: the UI's frame callbacks and events run
before the tick starts (`Renderer::run_ui_frame`), and the UI's world reads
the published snapshot (`ui_world`). Test runs (LAN games, fast matches,
`--play-for`) install a check that fails the run if the sim's Lua state is
used off its thread (`lua/lua_thread_check.hpp`): that is how the UI reading
the live sim from a frame callback, which desynced network games, was found.

## Sped up, late in a match (2026-10-19)

Ryan: at speed the units "bounce", and the game slows as units pile up.
Measured on the eight-AI match at rate 6 (about 4x), `--play-for`; the
frame line now also says how evenly the drawn game time ran: frames it
**held** (stood still while the game ran) and how much its pace changed
from frame to frame (`Window::note_drawn_time`). `--profile-frames` profiles
the frames with the sim left on its own thread (`--profile` puts it on the
main one, to time its zones).

**The bounce.** Sped up, a sim job runs a frame's several ticks and
publishes the last. The clock crossed those ticks in one tick's time, then
held at the newest until the next job: move, stop, move. `TickClock` now
keeps the drawn time a set distance behind the newest tick (three quarters
of a publish's ticks, averaged), its pace eased toward that distance and
slowing as it nears the newest; `WorldHistory` keeps three published ticks,
and the frame draws between the two either side of the drawn time. The
clock runs on the frame's wall time (`drawn_dt`), and moves before the view
is taken (it moved after, in `start_sim`, on the frame collecting a job).

| Clock (three minutes at 4x) | Held frames | Pace change a frame |
| --- | --- | --- |
| one tick per publish (before) | 3,049 | 130% |
| paced, three ticks kept | 238 | 15% |

At 1x and 2x: 15 and 17 held frames, 2-3%.

**The frame's own hitches.** `UIRenderer::update` scaled each quad in the
instance buffer after writing it: a read of GPU (write-combined) memory,
uncached, for every rect, about 7 ms a frame of 1,500 quads (the minimap's
unit marks grow with the armies). Scaled before it is written: the UI's
update went from 8.0 to 1.2 ms a frame, and frames over 33 ms from about
400 in three minutes to under 20.

**The sim, late (about 2,000 units, 26,600 entities, per tick).** Copying
the world for the renderer 30 ms (once a job now, not every tick:
`stage_events` each tick, `stage_world` at the job's end); Lua (the AIs)
26; orders 19 (patrol 7.6, move 4.4, building 3.3); visibility 14;
manipulators 8; weapons 4; the Lua collection 45 every 70 ticks. Path
search for an unreachable goal searched all the unit could reach (36,000
cells, 5% of requests, 90% of the cells searched); the component labels now
send it to the reachable cell A* would have ended at
(`Pathfinder::closest_reachable`), 81% fewer cells searched and no more
77 ms ticks. Five minutes at 4x against the engine before: 10,847 ticks
played against 9,833, frames over 50 ms 2 against 234.

## Ideas not yet done

- Props: the snapshot and `update_entities` visit every prop every tick;
  a prop that hasn't moved, fallen or changed could keep its record and be
  skipped.
- The replay checksum walks every entity every tick again.
- `CategoryMatcher` compiled again on each call; `GetBlueprintStat`
  building a string set per blueprint.
- Visibility (14 ms late): a hash map of every unit's recon rebuilt each
  tick, and each blip's blueprint id copied each tick.
- Manipulators (8 ms late): every unit's every manipulator, idle or not.
- The world capture's effects (9 ms of its 30): each effect's frame and
  beam worked out from its bones every capture.
- The Lua collection's stop-the-world mark (every 70 ticks, 35-95 ms): only
  an incremental or generational collector shortens it (M224h).
