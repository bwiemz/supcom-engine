# M223b: The Renderer Benchmark — Design

**Status:** 2026-09-30, built and measured (below). Roadmap Phase H (performance and
scale), after M223a (the sim benchmark). It decides between M225 (render threading,
asynchronous work) and M226 (instancing, descriptor indexing, fewer state changes).

## Why

M223a times the sim; nothing times a frame. M225 and M226 are both large, and which
one pays depends on where a frame's time goes:

- in recording commands on the CPU (the case for M226's batching);
- in the CPU waiting for the GPU (the case for M225's overlap);
- or on the GPU itself (neither: the shaders or fill).

Without a pinned visual workload, that is a guess.

## What it is

**`--render-bench <report.json>`**, in the windowed game.

- It runs the game loop the golden captures use (`--screenshot`): a fixed frame clock
  and no mouse. On top of that it sets a fixed window size and vsync off.
- A scene is a saved game and a camera path. The engine loads the save (M208c's
  snapshot restore, well under a second), then renders frames along the path while the
  sim runs on at 10 Hz, so effects keep emitting and units keep moving. It then writes
  the report and exits.
- Each frame is measured by clocks and counters only: the game is the one it would be
  without them.

**The report, per frame and summarised (p50/p95/p99/max):**

| Metric | How | What it tells |
|---|---|---|
| CPU frame time | `render()`'s own time, and its profiler zones (`unit_update`, `ui_update`, `shadow_pass`, `main_pass`, `submit`, ...), with the profiler's overlay hidden | where the CPU's time goes |
| CPU wait for the GPU | `Render::gpu_wait` (the fence) | whether the CPU idles on the GPU |
| GPU frame time | Vulkan timestamp queries at the command buffer's start and end, read back a frame later | the GPU's own cost |
| Draw calls, pipeline binds, descriptor-set binds, descriptor writes, push constants | counted by thin wrappers (`vk_cmd.hpp`) the renderer's 127 `vkCmd*` and descriptor-write calls go through | submission pressure and state churn |
| Primitives and shader invocations | a pipeline-statistics query per frame, when the device supports it | scene complexity on the GPU |
| Mesh instances (units and props drawn), particles, beams, trail segments, strategic icons | the renderers' own counts | what the scene holds |
| VRAM | VMA's heap budgets (`vmaGetHeapBudgets`): the bytes allocated, the blocks holding them, the budget | resource pressure |

The report also records the scene, its focus and resolution, the ticks it began and
ended at and the checksum at the end, the GPU's name, and the build.

**Three scenes**, from the pinned sim benchmark's game (SCMP_009, four AIs,
seed 4242), saved headless with `--save --save-at`:

- **Battle** (tick 6,000). The camera follows the map's busiest fight: the 64×64
  square with the most units of two or more armies, found from the save, so the
  choice is deterministic. Mid zoom, a slow pan and orbit, 600 frames.
- **Late battle** (tick 18,000). The same rule, 2,000+ units on the map, zoomed out
  enough to hold 500+ of them and their effects.
- **Strategic** (tick 18,000). The whole map in view: the strategic icons, every
  entity, the minimap. A slow zoom from the map's extent to half of it.

**Fixed settings:** 1920×1080 offscreen, vsync off, the fixed frame clock. The rest are
the defaults of the benchmark's own user folder (`bench/saves/user`, which holds no
preferences): fidelity high, fog of war as the player. The saves live beside the
goldens: they are the machine's, since snapshots are signed per installation.

**`tools/bench.py`** gains the render scenarios (`render-battle`, `render-late`,
`render-strategic`), with the same `run`, `compare` and `check`:

- **Compared:** CPU and GPU frame p50 and p95, and the VRAM allocated, each within the
  tolerance (15%). The p99s are reported but not held: across runs of a scene they move
  20-50% (a frame or two in 600).
- **Must match exactly:** the scene, the tick it starts at, the checksum it ends at,
  and the GPU. A change is a different workload, "not comparable", as another
  checksum is for the sim.
- **Baselines** are per machine, build type and device. The saves live with them
  (`bench/saves/`), made once by the headless `--save`. Saving and loading share one
  `--user-dir`: its key signs the snapshot, and a load without the key catches up by
  replay instead of restoring.

## Decisions

- **The real game loop, not a test harness.** `OffscreenShots` builds the scene
  per shot, and the render tests skip the UI and the game interface. Players pay for
  both, so the benchmark renders what the game renders, UI included.
- **Saves, not replays.** Reaching tick 18,000 takes about two minutes of sim. A
  restored snapshot takes under a second, and the render bench measures frames, not
  the catch-up.
- **The sim keeps running.** A frozen tick would leave the particles' emission and the
  interpolation idle, and those are part of a real frame's cost. The frame's time is
  `render()`'s alone, so the sim's ticks don't blur it (M223a times those).
- **GPU timing is optional per device.** No timestamp support (or a
  `timestampPeriod` of 0) reports GPU time as absent, not zero. The same goes for
  pipeline statistics.
- **Not in CI**, for M223a's reasons: no game data, and shared runners have no GPU.
  The comparison rules get a data-free self-test (`arch.bench_self_test` grows).

## What it should answer

The first report goes in this document, with a verdict on the three questions under
*Why*:

- CPU record time against GPU time, per scene;
- how much of the CPU time is waiting (`gpu_wait`);
- draws and binds per frame, against the instances drawn.

If the late battle is CPU-bound, with thousands of draws or binds per frame, M226
comes first. If the CPU mostly waits on a GPU busy the whole frame, the work is in the
shaders or fill, and neither M225 nor M226 comes first. If both are busy but serial,
M225's overlap pays.

## Tests

- **Unit:** the percentiles, p95 included (M223a's `tick_stats`, reused for frames);
  the scene rule (the busiest square two armies share, civilians not counting, ties to
  the lower row) on a synthetic world; the scene names.
- **`bench.py --self-test`:** the render scenarios' comparison: within tolerance,
  slower, a different workload, a missing GPU time.
- **By hand:** each scene run twice against its baseline, for the run-to-run noise.

## First measurements (2026-09-30, Release, NVIDIA RTX PRO 4500 Blackwell)

600 frames each, after 120 of warm-up, at 1920x1080.

| Scene | CPU `render()` p50 / p95 / p99 | GPU p50 / p95 | Draws | Set binds | Mesh instances | Particles | VRAM allocated |
|---|---|---|---|---|---|---|---|
| battle (tick 6,000) | 6.0 / 9.0 / 10.7 ms | 0.81 / 0.90 ms | 1,042 | 2,159 | 410 | 5,850 | 347 MB |
| late (tick 18,000) | 7.3 / 12.8 / 14.1 ms | 0.78 / 0.86 ms | 742 | 1,610 | 1,356 | 2,036 | 347 MB |
| strategic (tick 18,000) | 7.6 / 13.5 / 16.1 ms | 0.52 / 1.97 ms | 752 | 799 | 8,192 (the cap) | 8,709 | 361 MB |

The CPU frame's split (means, late scene):

| Zone | ms |
|---|---|
| `ui_update` | 3.03 |
| `unit_update` | 2.30 |
| `runtime_decals` | 0.72 |
| `overlay_update` | 0.44 |
| `particle_update` | 0.25 |
| `main_pass` + `shadow_pass` + `submit` (the recording) | 0.27 |
| `gpu_wait` | 0.00 |

- **The GPU is nearly idle:** under 1 ms of a frame (2 ms at p95 at the whole-map
  zoom). The CPU never waits on it.
- **Recording is not the cost:** 742-1,042 draws and 800-2,200 descriptor-set binds are
  recorded in about 0.3 ms.
- **The cost is the CPU's per-frame updates:**
  - `ui_update` (3 ms): retail's UI scripts' animations and OnFrame handlers, the
    control-tree walk and quad building, the minimap's paint.
  - `unit_update` (2.3 ms): each mesh instance and its bone poses.
  - The runtime decals (0.7 ms).
- **Noise:** across runs of a scene, CPU p50 moves under 10% and GPU p50 about 10%;
  p99 20-50%.
- **Found on the way:** the whole-map view hits the unit renderer's fixed
  `MAX_INSTANCES` (8,192). Meshes past it are skipped silently, so some units and props
  at that zoom are not drawn. To fix on its own.

## Verdict

**M226 doesn't come first, and nor does M225 as threading the submission.** Batching
draws, instancing more and descriptor indexing would save part of 0.3 ms, and the GPU
has room to spare.

What pays is the CPU's per-frame update work:

1. **Split `ui_update`** into retail's Lua (animations, OnFrame handlers) and the
   engine's own walk and quads, so the engine's part can be optimised. Retail's scripts
   cost what they cost, as in Moho.
2. **Make `unit_update` cheaper:** reuse static props' instances and poses across
   frames, and pose units in parallel. The latter is the useful part of M225.
3. **Fix the instance cap.**

Then measure again with this benchmark. On this machine the frame's p50 is 6-8 ms and
its p95 9-14 ms, against a 16.7 ms frame at 60 fps. A slower CPU, or a weaker GPU at a
higher resolution, may tell a different story. The benchmark runs anywhere with game
data, and its baselines are per machine and device.

