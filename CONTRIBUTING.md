# Contributing to OpenSupCom

OpenSupCom reimplements Moho, the engine of *Supreme Commander: Forged Alliance*, and runs the game's own Lua on it. This guide covers how changes are made, checked and reviewed. For building and running, see the [README](README.md). For where the project is heading, see [docs/ROADMAP.md](docs/ROADMAP.md) and [docs/current-state.md](docs/current-state.md). Questions, and ideas to talk over before writing code, go in [Discussions](https://github.com/bwiemz/supcom-engine/discussions).

## The ground rules

The full statement of what may enter the repository, where knowledge of the game may come from, and what the engine promises to run is [docs/provenance-and-compatibility.md](docs/provenance-and-compatibility.md). In short:

- **Never commit game data.** FA's assets are proprietary. Keep them out of the repository, including extracted files, screenshots of game art and golden images (see *Goldens* below). Tests that need the game find it on the developer's machine and skip (exit 77) when it isn't there.
- **FA's scripts decide; the engine provides.** When retail or FAF Lua implements a rule (victory, score, AI, UI, wrecks), the engine supplies the primitives Moho did and doesn't decide the outcome itself. Two referees disagree in subtle ways. Before adding engine logic, check whether a script, including one run through a hook, already does it.
- **Match Moho, not a guess.** A binding should behave as Moho's does, as far as scripts can observe. Retail FA 3599 is the reference, and FAF must keep working. When unsure, read the retail Lua that calls the binding, check FAF's engine annotations, or run the real game.
- **One logical change per commit**, with a message that says why.

## Workflow

1. Branch from `main` (`feat/…`, `fix/…`, `docs/…`). If you work alongside other checkouts, use a separate `git worktree` rather than switching branches under someone else.
2. Make the change with its tests (see *Tests*).
3. Run the checks below.
4. Update the docs the change affects:
   - the milestone row in `docs/ROADMAP.md`;
   - `docs/current-state.md` when a capability or a known gap changes;
   - a design note in `docs/plans/` for anything architectural.
5. Open a PR. CI builds on GCC, Clang, ASan+UBSan and MSVC, and runs the data-free tests and the static checks.

## Style and static checks

The code is C++20. `.clang-format` records the style: 4-space indent, 100 columns, pointers bound to the type (`T* p`), and unindented namespaces and case labels.

- **Formatting applies to the lines you change.** Untouched code is never reformatted wholesale, so blame stays useful.

  ```bash
  tools/check_format.sh              # lines changed since you branched from origin/main
  tools/check_format.sh --fix        # format them (stage your changes first)
  ```

- **clang-tidy is a ratchet.** `.clang-tidy` enables checks that find real bugs or waste: `bugprone-*`, `performance-*`, the core clang analyzers, and a few modernize and readability checks. The findings that existed when the ratchet started are listed, per file and check, in `tools/clang_tidy_baseline.txt`, and the count may only go down.

  ```bash
  tools/clang_tidy_ratchet.py -p build/linux-debug           # fails on a new finding
  tools/clang_tidy_ratchet.py -p build/linux-debug --update  # after fixing some: lower the baseline
  ```

  To run it through CTest, configure with `-DOSC_LINT=ON` and run `ctest -L lint`.
- **Pin the tool versions.** The style and the baseline were set with **LLVM 22**, and other versions format and diagnose differently. CI installs LLVM 22 for these checks.
- **Python tooling** (`tools/`, `tests/integration/*.py`) uses ruff and basedpyright, at the 100-column line length set in `ruff.toml`.

## Tests

There are several layers. Use the lowest one that can show the behaviour.

| Layer | Where | Runs |
|---|---|---|
| Unit (Catch2, no game data) | `tests/test_*.cpp` | everywhere, and in CI |
| Two-process multiplayer | `ctest -L mp` | everywhere, and in CI |
| Data-backed modes (`osc_integration --<name>-test`) | `tests/integration/runner/` (`integration_tests.cpp`; the flag table in `test_modes.cpp`), listed in `tests/integration/data_tests.cmake` | machines with FA |
| Regression gate | `ctest -L gate` | machines with FA; run it before every PR |
| Cross-OS replay | `tools/cross_os_replay.py` | machines with FA and Wine; run it before merging a change to the sim's arithmetic |

- **Test first.** Write the failing test, watch it fail for the right reason, then fix. A bug fix comes with the test that would have caught it.
- **Data-backed modes** exit non-zero on a failed check. In test modes, a Lua error in a script thread also counts as a failure, so a mode passes only if the scripts ran clean. Add a new mode to the gate list in `data_tests.cmake` once it passes on retail.
- **Windowed test modes** must render offscreen. Add them to `offscreen_capture` in `main.cpp`; a shown window can block forever when there is no compositor or the screen is locked.
- **Goldens.** `--golden <name>` captures a frame on a fixed clock and compares it with `$OSC_GOLDEN_DIR/<name>.png`. These images contain game art, so they live outside the repository. A missing golden is skipped. Updating one (`--golden-update`) is a deliberate act: inspect the diff first. A golden of a screen past the main menu opens it with `--click <label>`, repeatable: the buttons so labelled are clicked in turn, as a player clicks them, and a button that never takes its click fails the run. `--click-at <x>,<y>` clicks a point in turn with them, a frame after the click before (a scrollbar's track, say). `--mouse "<when> <verb> <what>"`, repeatable, plays a player's mouse through the window's input, a step at a time, so a golden can hold a button down: `<when>` is a frame (as `--screenshot-frame` counts) or `t<tick>`; the verbs are `move <x>,<y>` (pixels) or `move w<x>,<z>` (the ground there), and `press`/`release` of `left`, `right`, `middle`, `shift`, `ctrl` or `alt`. A step that never comes fails the run.
- **Retail references.** `tools/retail_ref/retail_ref.py` asks retail FA itself, for a behaviour or a screen to check the engine against: it starts the game (through Wine where it isn't Windows) with a hook that clicks its front end's buttons by their labels, as `--click` does, runs the given probes and logs what they find, and can capture its window (`--screenshot`). A probe is a Lua file defining `Probe(log, args)`; `tools/retail_ref/probes/` has two, of texture sizes and of font metrics. The game runs on a clean profile, and the player's `Game.prefs` is put back after. Under Wine the prefix needs Microsoft's D3DX (`winetricks d3dx9_35`; the tool checks), and with no desktop it runs under `xvfb-run` (in Docker, `docker run --init`).
- **Replays.** `--record <file>` records a game and `--replay <file>` plays it back headlessly, checking every tick; the first differing tick is reported. `--watch <file>` opens one in the game. An interactive game leaves `LastGame.oscreplay` in its profile's replays folder: attach it to a gameplay bug report. `data.replay_roundtrip` and `data.replay_flow` (gate) hold this.
- **Cross-OS determinism.** `tools/cross_os_replay.py --run-id <CI run> --linux-exe build/linux-debug/opensupcom -- <game args>` fetches that run's Windows build (the `opensupcom-windows` artifact), records a game with the Linux build, and plays it with both. The Windows build runs under Wine on the same game data. The two checksum traces must be identical.
- **Render refactors.** `--render-dump <file>` renders a scripted scene offscreen and writes everything the renderers generate. Two runs of the same build are byte-identical, so dump before and after a render-path change and compare the files with `cmp`.

## Engine conventions that aren't obvious

**The sim is deterministic; keep it that way.**
- Everything that changes sim state happens inside `SimState::tick()`, or through the command stream.
- Don't let hash-map iteration order or pointer order decide a sim outcome.
- Sim code (and anything that feeds sim state) computes `sin`, `atan2`, `pow` and the other transcendentals with `osc::dmath`, not `<cmath>`: the platforms' libms differ in the last bit. `sqrt`, `floor`, `ceil`, `fmod` and `fabs` are exact everywhere.
- Don't use a random source other than the sim's (`SimState::random()`, which scripts' `Random` and `math.random` use too).
- To find where two runs part ways, compare `--checksum-trace` files with `tools/checksum_diff.py`. It names the first divergent tick and whether the random stream, the armies or the entities differ. `data.determinism` in the gate plays a game twice this way.
- The renderer and the UI read per-tick snapshots (`sim::WorldSnapshot` / `FrameView`), never the live sim. The draw path doesn't include sim headers.

**Lua 5.0 and LuaPlus.**
- The VM is Lua 5.0, so `lua_getfield`, `lua_objlen` and friends don't exist.
- `lua_isstring` is true for numbers; use `lua_type(L, i) == LUA_TSTRING`.
- Copy the result of `lua_tostring` before popping it.
- Use `lua_rawget`/`lua_rawset` on the registry and on `_G`, because `config.lua` locks globals.
- LuaPlus lets scripts index `nil` and booleans (the result is `nil`), and retail relies on it.

**Bindings.**
- A script object reaches its C++ object through `_c_object` (a light userdata). Handles are checked against the sim generation (`_c_sim_gen`), so a stale handle from an earlier session resolves to nothing.
- Never keep a registry ref or an id as a long-lived identity without a serial or generation, because refs and ids are recycled.
- Armies are 0-based in C++ and 1-based in Lua.

**Hooks.** Retail applies `/schook` hook files to modules as they load, and each hook runs as its own chunk. A mod or FAF may change what a file does at runtime, so read the hooked result, not only the base file.

## Commit messages and PRs

- Write the subject in the imperative, scoped by area: `sim: …`, `renderer: …`, `lua: …`, `docs: …`.
- In the body, say why the change was needed and what behaviour changed. Record what a reviewer can't see in the diff, such as measurements, the retail behaviour matched, or the checks run.
- In the PR description, list what was tested and what was left for later.
