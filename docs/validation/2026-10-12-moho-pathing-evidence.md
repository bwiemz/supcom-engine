# Moho pathing: the evidence for making it the default (2026-10-12)

The roadmap's P1 asks for the evidence that `--moho-pathing` (Moho's pathing by footprint class, #415–#418) is ready to replace the grid's as the default: crowded movement, water transitions, saves and current performance. This record gathers it on one head.

**Head:** main `3be4d6a5` with this PR's two new modes, `--shore-test` and `--choke-test`. The engine's code is main's. The counts and ticks below are from those runs' logs (each mode logs them); the tests assert the rules, not the counts.
**Build and data:** Release; retail FA (Steam), pinned with `OSC_FA_PATH`. Linux 7.2.9, 16 threads.

## Summary

| Area | With Moho's pathing | With the grid's (today's default) |
|---|---|---|
| The gate's data modes | 202 of 202 | 202 of 202 |
| Water transitions | Hover over the water, amphibious along the seabed, land round: as Moho's footprints allow | The same |
| A crowd through a gap | None through the wall; 4 of 24 stop short of the gap (below) | All 24 across; 6 of them through wall cells |
| A class that ignores structures | The Monkeylord walks over the wall, as retail's `footprints.lua` has it | Round the wall's end |
| Saves and replays | Identical after loads, snapshot and by-replay; deterministic across processes | The same |
| Sim time, late game | 81 s (546 units) | 139 s (874 units) |

## The gate's data modes with the flag

Every data mode of the gate (`OSC_DATA_TESTS_GATE`, 202 with this PR's two) was run with `--moho-pathing` added: `osc_integration --map SCMP_009 --<mode> --moho-pathing`, eight at a time. **All 202 pass.** It was 189 of 195 at ee8ab53b and 195 of 195 once #448 gave the six modes whose numbers the flag changes their flag-aware checks.

Without the flag, the gate (`ctest -L "gate|mp|golden|arch"`, 268 tests with this PR's four) passes but `data.golden.build_drag`, whose stale click #600 fixes.

Since #448 the gate runs the modes whose checks the flag changes with it as well (`data.moho.*`): `drive-test`, `steer-test`, `crowd-test`, `weapon-test`, `range-test` and `factory-rally-test`, and from this PR `shore-test` and `choke-test`.

## Water transitions (`--shore-test`)

**The scene:** a bay on Seton's Clutch's west edge, at x 40–48, with dry land to z 581, water up to 5.6 deep to z 638, then dry land again. A hover tank (`uel0203`), an amphibious one (`url0203`) and a land one (`uel0201`) are sent across it side by side.

| | The hover tank | The amphibious tank | The land tank |
|---|---|---|---|
| Its layers on the way | Land, then Water, then Land | Land, then Seabed, then Land | Land only, round the bay |
| Arrived, Moho's pathing | tick 197 | tick 197 | tick 234 |
| Arrived, the grid's | tick 197 | tick 197 | tick 228 |

**Most of the map's shores are closed to both units, under either pathing:**
- On the first crossing tried (the strait at z 520) all three units drove some 500 units round by land.
- Moho's fit test (`OCCUPY_MobileCheck`, ported as `map_caps`) shows why: the banks fall 1–1.3 a cell. That is steeper than the 0.75 MaxSlope of the hover's class (WaterLand) and of the amphibious one's.
- Where the footprint still touches dry ground, the water cap is gone as well. So a band one or two cells wide along the shore is closed to both, and they go round. The grid's pathing closes steep cells whatever the water above them, so it agrees there.
- A scan of the whole map for straight crossings open to both footprints found two, this bay and its mirror on the east edge. This is a fact about Seton's Clutch's shores, not a bug: hovers and amphibious units enter water where its banks are gentle.

## A crowd through a gap (`--choke-test`)

**The scene:**
- A wall of 1×1 segments (`ueb5101`) along z 175 from x 650 to 790, with a gap 4 wide at x 718–722.
- 24 Strikers (`uel0201`, 1×1) south of it, sent to one point north of it.
- A Monkeylord (`url0402`) sent straight at the wall, 40 units from the gap.

**With Moho's pathing:**
- 20 Strikers go through the gap, the last in by tick 298. None crosses a wall cell: Moho's drive moves a unit only where its footprint fits.
- **4 stop short of the gap, against the wall, their orders ended.** A trace shows why:
  - after three failed searches for the way to its next cell, each unit asks for a fresh path (`RequestPath(1)`, an Initial search);
  - that search counts stopped units as blockers. With the gap full of units waiting their turn, it reaches nothing beyond the unit's own cell;
  - its centre goal cell is held by the crowd at the goal, so the path stays that one cell, and the unit arrives where it stands.
- That is faf-re's `CAiPathNavigator::OnEvent` as the engine has it, rule for rule; Moho also marks such a unit `ProblemGettingToGoal`, which nothing in retail reads. Whether the original game strands units the same way is for the original-Moho comparison (roadmap P2).
- The Monkeylord walks over the wall at x 679, in by tick 250. Its class, Amphibious3x3, has IgnoreStructures in retail's `footprints.lua`, and so do all of retail's large land units (3×3 and 6×6, all amphibious). In retail's `footprints.lua` and blueprints, the other land units resolve to 1×1 classes, so a gap in a wall that a 1×1 unit takes turns none of them away; terrain alone narrows the way for them.

**With the grid's pathing:**
- All 24 Strikers get across, the last in by tick 220.
- **6 of them cross through wall cells**, at x 715.3–717.6 and 722.6. The grid paths units as points on 2-unit cells, and its smoothed paths and drive cut the wall's corners.
- The Monkeylord goes round the wall's west end (x 649), in by tick 361.

## Saves and replays

- **In the gate:** `data.save_load_moho_pathing` saves a flag-on game holding paths being followed, searches waiting and in flight, and dirty clusters; each load plays on identically.
- **With #603 (open):** a replay and a save's history record the switch, and `data.replay_roundtrip_moho_pathing` and `data.save_load_replay_moho_pathing` play back without the flag.
- **In the 3be4d6a5 record:** two processes played a flag-on game identically for 1,500 ticks (`determinism.py`, checksum `7efca2dd`), and a by-replay save/load stayed identical after each load.

## Performance (main 3be4d6a5)

`tools/bench.py` scenarios, Release, four retail AIs on SCMP_009, seed 4242, run one at a time on a quiet machine:

| Scenario | Sim time | Tick mean / p50 / p99 | Path searches (cells expanded) | Then (2026-10-07) |
|---|---|---|---|---|
| `early` | 7.5 s | 1.26 / 0.92 / 9.6 ms | — | 7.4 s |
| `early-moho` | 8.0 s | 1.34 / 0.96 / 10.5 ms | 1,704 (336 k) | 10.4 s |
| `late` | 139 s | 7.7 / 6.0 / 41 ms | — | 91 s |
| `late-moho` | 81 s | 4.5 / 3.7 / 36 ms | 35,097 (23 M) | 184 s |

- **The two pathings play different games**, so these numbers aren't a like-for-like comparison. The late game ends with 874 units without the flag and 546 with it.
- **The default late game ran outside `bench.py`.** It now meets retail's engineer-platoon race (`aiattackutilities.lua:952`), so its run exits 1 and `bench.py` drops the report. It was run directly with `--bench`.
- **The default late game has slowed.** It takes 139 s against 91 s at the 2026-10-07 baseline, with fewer units (874 against 1,418). That is a performance regression since then, to be found with P2's fresh benchmarks.
- **#591 changed both games:** retail's AIs build about half the units now (see the [3be4d6a5 record](2026-10-12-3be4d6a5.md)). The bench's baselines need recording again.

## Long games with the flag (the 3be4d6a5 record)

| Data | Seed | Wall time | Units at the end | Script errors |
|---|---|---|---|---|
| retail | 4242 | 84 s | 546 | none |
| retail | 7777 | 290 s | 770 | none |

Both games ran three at a time beside others.

## What stands between it and the default

1. **Stranded units at a crowded gap** (4 of 24 above). The port follows faf-re's navigator rule for rule; only the original game can tell whether Moho strands them too.
   - **Answered, 2026-10-13:** the original game gets all 24 across, and none of its units stands still for more than 17 ticks (`tools/moho_probes`).
   - The difference is in the motion, not the navigator. A Moho unit whose step won't fit is bounced back (`CUnitMotion::CalcMoveCommon`, `AddImpulse` at its top speed x 0.01), and its steering asks for the way afresh once the push dies down. A unit against the wall is therefore never still for the 30 ticks after which its navigator gives up.
   - The engine's drive now bounces it too. With Moho's pathing 23 of 24 get across; one still stops short.
   - **Closed, 2026-10-15:** the last one was the navigator, not the motion.
     - What it was: pressed against the wall's face, its continuation searches found nothing nearer its next cell than its own, twice running, so it gave up.
     - What the original showed: its Strikers never get pinned there. `tools/moho_probes/navigator` shows a lone Striker's target cells turn toward the gap ten cells before the wall, where faf-re's navigator would drive at the far side's portal until the wall refused it.
     - Five functions in the shipped exe differ from faf-re's text: `CAiPathNavigator`'s `UpdateCurrentPosition`, `TryAdvanceTargetPoint`, `RequestContinuationPath` and `OnEvent`, and `CAiPathFinder::ShouldSearchRect`. The target advances only halfway to it. Every 7 ticks the unit looks down the way to its target. With nothing ahead reachable straight, it asks the way cell by cell. The remembered search windows count a touching cluster.
     - Ported from the binary: all 24 get across in the choke scene. In 24 variants with the crowd's spawn points jittered, 1 of 576 Strikers stops short (43 before). In the original's six variants of `tools/moho_probes/crowd`, 1 of 144 did.
2. **The games differ.** The AIs play otherwise with Moho's pathing, so the goldens and benchmarks that hold a pinned game will need re-recording when the default changes.
3. **What the switch would retire:** the grid's corner-cutting through wall cells (6 of 24 above).

Not covered here: formation moves through a gap, naval movement round structures, and a human watching flag-on games.
