# M192 — Split the executable

Status: step 1 done, 2026-09-24. Phase C (architecture seams), brought forward after an external review of the codebase.

## Why

- **The game ships its tests.** `opensupcom` is built from `main.cpp` (4,679 lines), `integration_tests.cpp` (18,476) and `audio_data_test.cpp`. About 130 `--*-test` flags live beside the game's own.
- **`main()` is 3,080 lines,** and the tests are woven through it:
  - 130 `parse_flag` lines, and an `any_test` expression over them;
  - an `if` per test mode;
  - ten modes written inline (smoke, full-smoke, draw, stress, dual-state, construction, phases 2–5, lobby flow);
  - test probes inside the game loop (`--interp-test`, `--render-dump`);
  - test-only branches in the boot (which army is AI, whether the game UI is built).
- **What that costs:**
  - Every test change rebuilds and relinks the game.
  - A player's binary answers `--damage-test`.
  - Changes to the loop or the boot collide with test edits.
  - Nothing can reuse the boot without the tests.

## The steps

### 1. The seam: `osc_app`, `opensupcom` and `osc_integration`

- **`osc_app`** (`src/app/`, a library) is the engine as `main()` built it:
  - `app.cpp`: `osc::app::run(argc, argv, TestModes*)`. It is `main()`'s body without the tests: the boot, the front end, the session, the windowed loop and the headless run. It moved from `src/main.cpp` in a commit of its own, so history follows it.
  - `app.hpp`: `run` and the seam (below).
  - `support.hpp`: what both the game loop and the tests drive, declared from `app.cpp`:
    - argument parsing, `finish_test_run` and the "no game data" exit code (77);
    - UI frames and the sim beat;
    - world-UI setup, selection and command-mode dispatch, and SimCallback submission;
    - the reload into a new game, and the seeds.
- **`opensupcom`** is `src/main.cpp`: `return osc::app::run(argc, argv, nullptr);`.
- **`osc_integration`** (`tests/integration/runner/`) is `run(argc, argv, &modes)` with the test modes:
  - `integration_tests.cpp` and `audio_data_test.cpp` (moved);
  - the dispatch;
  - the inline modes;
  - the probes;
  - the LAN and MP harnesses (`--mp-host`, `--lan-host`, `--lan-ui-test`).
- **CTest runs the runner** for the `data`, `gate` and `mp` labels. The game keeps what a player or developer uses:
  - `--map`/`--ticks`, `--replay`/`--record`/`--watch`, `--ai-skirmish`;
  - the traces, `--screenshot`/`--golden`, `--binding-coverage`;
  - `--replay-flow-test`, which is woven into the loop's replay handling.

**The seam** is `osc::app::TestModes`. Its hooks sit where the modes' code stood in `main()`:

| Hook | Where | Modes |
|---|---|---|
| `parse` | after the game's own flags | all: whether one was asked for, and how to boot (headless; ARMY_2 as AI; the game UI) |
| `before_boot` | before any engine init | the LAN and MP harnesses (`--mp-host`, `--lan-host`, `--lan-ui-test`) |
| `before_init` | FA found, before the engine's init | `--audio-data-test` |
| `front_end` | the no-map boot, before the loop | `--lobby-flow-test` |
| `frame_view` / `frame_rendered` / `frames_done` / `after_window` | the windowed loop | `--interp-test`, `--render-dump` |
| `headless_first` | the game booted, before the run's own headless modes | `--full-smoke-test`, `--smoke-test`, `--draw-test`, `--stress-test` |
| `headless` | after `--ai-skirmish` and `--ticks` | every other mode (a table of flags and test functions, and the modes with code of their own) |

`--draw-test` and `--stress-test` ran just after `--ai-skirmish` in `main()`. They now run just before it, which matters only if both are given.

A hook gets an `Engine`, a set of references to what `run` built:
- the Lua states and the sim;
- the VFS and the blueprint store;
- the UI registries, the game-state manager, the world-UI provider and the sound manager.

The tests use nothing else. Nothing in `integration_tests.cpp` refers to `main.cpp` today.

### 2. Decompose `app.cpp`

- **Files:** `app.cpp` splits into files (`cli`, `frames`, `reload`, the boot, the windowed loop).
- **One object:** `run`'s boot, windowed loop and headless run become functions over one `Engine` object, not one function's locals.
- They are pure moves, checked as M193 was.

## Proof

- **The gate:** every mode passes through the runner (127 on retail). So do the golden captures and the MP pairs.
- **The oracle:** four-AI checksum traces from `opensupcom --ai-skirmish` are byte-identical before and after (the boot is unchanged).
- **The game's binary:** it holds no test symbols (`nm` finds no `osc::test::`). Given a test flag, it names the runner.
- **Cross-OS:** the replay on the PR's Windows build. CI's Windows artifact carries the runner too, for `cross_os_replay.py --direct`.

## Risks

- **A hook reached in the wrong place** changes when a test runs relative to the boot. Each hook sits exactly where its `if` stood, and the gate shows it.
- **Flags both sides read** (`--map`, `--ticks`, `--seed`, `--ai-personality`): the runner reads them from the same `argv`, as `main()` did.
