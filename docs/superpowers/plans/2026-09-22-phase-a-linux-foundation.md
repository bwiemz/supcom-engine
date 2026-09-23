# Phase A — Linux Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Linux a first-class build and runtime platform, boot retail (non-FAF) FA
data, and make the test suites machine-checkable. CI then guards all of it.

**Architecture:**
- **Platform layer.** A new `osc::platform` library holds OS differences: paths, signals and
  crashes, and Steam library discovery. The rest of the engine stops using `#ifdef _WIN32`.
- **Data compatibility.** It lives where it already does: VFS mounts, the init loader, the Lua
  lexer/parser, and `doscript`.
- **Verification.** Integration modes return exit codes and are registered with CTest under a
  `data` label. CI runs only the data-free suite.

**Tech Stack:** C++20, CMake ≥ 3.21 with presets, vcpkg manifest mode, Ninja, GCC 16 and
Clang 22 on Linux, MSVC on Windows, Catch2 v3, GitHub Actions.

**Spec:** `docs/ROADMAP.md` §5 Phase A (M175–M182) plus the audit findings in §2.

## Global Constraints

- **Game data.** Never commit or download FA game data. Tests that need it read `OSC_FA_PATH`
  and are labelled `data`.
- **Portability.** Every change builds on MSVC, GCC and Clang, and platform code lives in
  `src/platform/`.
- **Retail and FAF behaviour.** Retail behaviour must not regress FAF behaviour. FAF init paths
  keep working unchanged when `--init`/`--faf-data` are given.
- **Linux dev box.** Retail FA lives at `/mnt/extrastorage/SteamLibrary/steamapps/common/Supreme Commander Forged Alliance`
  with init `bin/SupComDataPath.lua`. `VCPKG_ROOT=$HOME/vcpkg`.
- **Build commands.** `cmake --preset linux-debug && cmake --build build/linux-debug && ./build/linux-debug/tests/osc_tests`.

---

### Task 1: Linux presets and GCC compile fixes (M175) — ✅ DONE `a052367`

- Added `linux-debug`, `linux-release` and `linux-asan` presets (Ninja, `x64-linux`), and
  restricted the MSVC presets to Windows hosts.
- Fixed a qualified definition inside `namespace osc::lua`
  (`moho_bindings.cpp: push_selected_units_for_ui`).
- Added a `<cstddef>` include before `pl_mpeg.h`.
- **Verified:** GCC 16 and Clang 22 build; 210/210 unit tests pass.

### Task 2: Case-insensitive VFS and glob mounts (M178 part) — ✅ DONE `7b640ef`

- Added `src/vfs/path_utils.{hpp,cpp}` with:
  - `wildcard_match(pattern, name)`
  - `has_wildcard(s)`
  - `resolve_case_insensitive(path) -> optional<path>`
  - `expand_glob(pattern) -> vector<path>` (sorted by lowercase name)
- `DirectoryMount` resolves through a lazy lowercase→real index.
- The init path table glob-expands its entries, and `io.dir` shares that expansion.
- `find_files` results are sorted in both mounts.
- **Tests:** `tests/test_vfs_paths.cpp`, 7 cases.

### Task 3: LuaPlus `#` comments and table size hints (M178 part) — ✅ DONE `00966ba`

- `llex.c`: `#` now starts a line comment.
- `lparser.c`: size hints `{&1&4 ...}` are accepted at the start of a constructor.
- **Tests:** two new cases in `tests/test_lua_state.cpp`.

---

### Task 4: Build hardening — warnings, sanitizers, missing includes (M175)

**Status:** ✅ DONE `b953107` — 19 warnings fixed (two real: snprintf truncation, FormPlatoon min count → M207); ASan+UBSan+LSan clean after `eb602a4`

**Files:**
- Create: `cmake/OscCompileOptions.cmake`
- Modify: `CMakeLists.txt` (include the module), every `src/*/CMakeLists.txt` (link `osc::warnings`)
- Modify (missing includes): `src/sim/economy_event.hpp`, `src/sim/ieffect.hpp` (`<algorithm>`), `src/sim/navigator.cpp` (`<algorithm>`), `src/renderer/renderer.hpp` (`<array>`), `src/renderer/unit_renderer.cpp` (`<cstring>`), `src/main.cpp` (`<variant>`, `<chrono>`), `src/renderer/texture_cache.cpp` (`<chrono>`)

**Interfaces:**
- Produces: CMake INTERFACE target `osc::warnings`, and cache variable `OSC_SANITIZE` (a `;`-list
  of `address`/`undefined`/`thread`) that applies to every target, vendored Lua included.

- [ ] **Step 1: Write `cmake/OscCompileOptions.cmake`**

```cmake
# Project-wide compile options. Warnings apply only to first-party targets
# (link osc::warnings); sanitizers apply to everything so ASan sees the whole
# process, including vendored Lua.
add_library(osc_warnings INTERFACE)
add_library(osc::warnings ALIAS osc_warnings)
if(MSVC)
    target_compile_options(osc_warnings INTERFACE /W3 /permissive- /utf-8)
else()
    target_compile_options(osc_warnings INTERFACE -Wall -Wextra -Wno-unused-parameter)
endif()

set(OSC_SANITIZE "" CACHE STRING "Sanitizers to enable (address;undefined;thread)")
if(OSC_SANITIZE AND NOT MSVC)
    list(JOIN OSC_SANITIZE "," _osc_san)
    add_compile_options(-fsanitize=${_osc_san} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${_osc_san})
endif()
```

- [ ] **Step 2: Include the module in the root `CMakeLists.txt`** before any `add_subdirectory`
  (`include(cmake/OscCompileOptions.cmake)`). Link `osc::warnings` PRIVATE from each first-party
  target.
- [ ] **Step 3: Build and record the warning count.** Run
  `cmake --build build/linux-debug 2>&1 | grep -c warning:`. Fix every warning in files this
  plan touches; log the rest as the baseline in `docs/current-state.md`. There is no
  `-Werror` yet: M194 ratchets it.
- [ ] **Step 4: Add the missing standard includes** listed under Files. They compile today
  only through transitive includes.
- [ ] **Step 5: ASan run.** Run
  `cmake --preset linux-asan && cmake --build build/linux-asan && ./build/linux-asan/tests/osc_tests`.
  Fix or document every finding (leaks from Lua error paths are expected until Task 9, so set
  `ASAN_OPTIONS=detect_leaks=0` until then).
- [ ] **Step 6: Commit** `build: warning flags, OSC_SANITIZE, missing standard includes`.

### Task 5: Platform library — paths (M176)

**Status:** ✅ DONE `c40e04c`

**Files:**
- Create: `src/platform/CMakeLists.txt`, `src/platform/paths.hpp`, `src/platform/paths.cpp`
- Modify: `CMakeLists.txt` (add_subdirectory before `src/core`), `src/lua/engine_bindings.cpp:l_SHGetFolderPath`
- Test: `tests/test_platform_paths.cpp`

**Interfaces:**
- Produces:
  - `osc::platform::KnownFolder { Documents, LocalAppData, Config, Cache, State }`
  - `std::filesystem::path osc::platform::known_folder(KnownFolder)`
  - `std::filesystem::path osc::platform::known_folder(KnownFolder, const EnvLookup&)`, where
    `EnvLookup = std::function<std::optional<std::string>(const char*)>`. This overload makes it
    testable without touching the real environment.
- **Linux mapping:**

  | Folder | Path |
  |---|---|
  | Documents | `$XDG_DOCUMENTS_DIR` or `~/Documents` |
  | LocalAppData | `$XDG_DATA_HOME` or `~/.local/share` |
  | Config | `$XDG_CONFIG_HOME` or `~/.config` |
  | Cache | `$XDG_CACHE_HOME` or `~/.cache` |
  | State | `$XDG_STATE_HOME` or `~/.local/state` |

- **Windows mapping:** `SHGetKnownFolderPath` (Documents, LocalAppData); Config, Cache and State
  map to LocalAppData.

- [ ] **Step 1: Failing test**

```cpp
TEST_CASE("known_folder honours XDG overrides", "[platform]") {
    auto env = [](const char* k) -> std::optional<std::string> {
        std::string key = k;
        if (key == "HOME") return "/home/u";
        if (key == "XDG_DATA_HOME") return "/data";
        return std::nullopt;
    };
    CHECK(known_folder(KnownFolder::LocalAppData, env) == "/data");
    CHECK(known_folder(KnownFolder::Config, env) == "/home/u/.config");
    CHECK(known_folder(KnownFolder::Documents, env) == "/home/u/Documents");
}
TEST_CASE("known_folder ignores relative XDG values", "[platform]") {
    // XDG spec: relative paths are invalid and must be ignored.
    auto env = [](const char* k) -> std::optional<std::string> {
        std::string key = k;
        if (key == "HOME") return "/home/u";
        if (key == "XDG_CONFIG_HOME") return "relative/dir";
        return std::nullopt;
    };
    CHECK(known_folder(KnownFolder::Config, env) == "/home/u/.config");
}
```

  Guard both cases with `#ifndef _WIN32`.
- [ ] **Step 2:** Run `osc_tests "[platform]"`. Expect a compile failure: the header is missing.
- [ ] **Step 3: Implement `paths.cpp`** with a POSIX branch (the env lookup above; fall back to
  `getpwuid(getuid())->pw_dir` when `HOME` is unset) and a Windows branch
  (`SHGetKnownFolderPath`, UTF-16→UTF-8). Link `shell32` and `ole32` on Windows.
- [ ] **Step 4:** Make `l_SHGetFolderPath` return `known_folder(...)` with a trailing `/` for
  `PERSONAL` and `LOCAL_APPDATA`. Delete the `#ifdef _WIN32` block in `engine_bindings.cpp`.
- [ ] **Step 5:** Run the tests and the retail headless boot (see Task 8). Expect the log line
  `checking /home/brandon/Documents/My Games/...`.
- [ ] **Step 6: Commit** `platform: known-folder paths (XDG on POSIX, Known Folders on Windows)`.

### Task 6: Platform library — crash handler and SIGPIPE (M176)

**Status:** ✅ DONE `40fa1c6`, review follow-up `ffe832d` (no logger calls in the signal handler)

**Files:**
- Create: `src/platform/crash_handler.hpp`, `src/platform/crash_handler.cpp`
- Modify: `src/main.cpp:1-5,807-815,1149-1151` (remove the Windows-only handler and call
  `platform::install_crash_handler()`), `src/sim/net_transport.cpp:57` (`send` flags)
- Test: `tests/test_tcp_transport.cpp` (new case: sending to a closed peer returns an error and
  does not kill the process)

**Interfaces:**
- Produces: `void osc::platform::install_crash_handler()`. It is idempotent.
  - **POSIX:** `SIGSEGV`/`SIGBUS`/`SIGFPE`/`SIGILL`/`SIGABRT` go to an async-signal-safe
    handler. It writes the signal name, then `backtrace()` + `backtrace_symbols_fd(…, STDERR_FILENO)`,
    flushes spdlog best-effort, then re-raises with the default action. `SIGPIPE` is set to
    `SIG_IGN`.
  - **Windows:** the existing `SetUnhandledExceptionFilter` body moves here.

- [ ] **Step 1: Failing test** in `test_tcp_transport.cpp`. Host and join on loopback, destroy the
  client, then call `host.send(...)` repeatedly for 200 ms. The test process must survive, and
  `send` must report failure or drop the peer. Without the fix the test binary dies from SIGPIPE.
- [ ] **Step 2:** Run it and confirm it dies (exit by signal 13).
- [ ] **Step 3:** Pass `MSG_NOSIGNAL` on Linux (`#ifdef MSG_NOSIGNAL`) and set `SO_NOSIGPIPE`
  where defined (macOS). Add `install_crash_handler()`, including `signal(SIGPIPE, SIG_IGN)`.
- [ ] **Step 4:** Run the tests and confirm they pass. Manually run `opensupcom --crash-test`, a
  hidden flag that dereferences null, and check that stderr shows a backtrace containing
  `main`.
- [ ] **Step 5: Commit** `platform: crash handler + SIGPIPE-safe sockets`.

### Task 7: Game data discovery (M177)

**Status:** ✅ DONE `06fe880`, VDF depth cap `ffe832d`

**Files:**
- Create: `src/platform/steam_library.hpp`, `src/platform/steam_library.cpp` (VDF parsing, Steam
  root discovery), `src/platform/game_install.hpp`, `src/platform/game_install.cpp` (the
  selection policy)
- Modify: `src/main.cpp:parse_args` (use `locate_game_install`, add `--print-install`)
- Test: `tests/test_game_install.cpp` with VDF fixtures in a temp dir

**Interfaces:**
- Produces:
  - `std::vector<std::filesystem::path> osc::platform::parse_library_folders_vdf(std::string_view text)`
  - `std::optional<std::string> osc::platform::parse_app_install_dir(std::string_view acf_text)` (the `"installdir"` value)
  - `std::vector<std::filesystem::path> osc::platform::steam_roots(const EnvLookup&)`. On Linux:
    `~/.local/share/Steam`, `~/.steam/steam`, `~/.var/app/com.valvesoftware.Steam/.local/share/Steam`.
    On Windows: registry `HKCU\Software\Valve\Steam\SteamPath`, then
    `C:/Program Files (x86)/Steam`.
  - `std::optional<std::filesystem::path> osc::platform::find_steam_app(uint32_t app_id, const std::vector<std::filesystem::path>& roots)`
  - `struct GameInstall { std::filesystem::path fa_path, init_file, faf_data_path; std::string source; }`
  - `std::optional<GameInstall> osc::platform::locate_game_install(const GameInstallHints& hints, const EnvLookup& env)`,
    where `GameInstallHints { std::optional<path> fa_path, init_file, faf_data_path; }` comes from
    the CLI.
- **Precedence:**
  1. CLI (`--init` / `--fa-path` / `--faf-data`)
  2. Env: `OSC_INIT_FILE`, `OSC_FA_PATH`, `OSC_FAF_DATA`
  3. FAF data dir, if it contains `bin/init_faf.lua`. Windows: `C:/ProgramData/FAForever`.
     Linux: `~/.faforever`.
  4. Steam app 9420, using its `bin/SupComDataPath.lua`

  If FAF is found but `fa_path` is unknown, fill it from the Steam lookup.

- [ ] **Step 1: Failing tests.**
  - Parse this machine-shaped VDF fixture (two libraries, app 9420 in the second) and expect both
    paths back.
  - An ACF fixture should give `installdir` = `Supreme Commander Forged Alliance`.
  - `locate_game_install` with a fake env and a temp Steam tree should return
    `source == "steam"` and `init_file` ending in `bin/SupComDataPath.lua`.
  - CLI hints override everything.
- [ ] **Step 2:** Run the tests. Expect a compile failure.
- [ ] **Step 3: Implement.** The VDF format is nested `"key" "value"` / `"key" { … }`. Write a
  ~60-line tokenizer that handles quoted strings with `\\` escapes, braces, and `//` comments.
  Do not add a dependency.
- [ ] **Step 4:** Wire it into `main.cpp`, replacing the hardcoded `C:/ProgramData/FAForever`
  defaults and the `fa_path.lua` backslash parser (keep that parser as the FAF fallback, moved
  into `game_install.cpp`). `--print-install` prints the chosen install and every candidate
  checked, then exits 0.
- [ ] **Step 5:** Run the tests. Running `./build/linux-debug/opensupcom --print-install` on this
  machine must print `source=steam` with the path above.
- [ ] **Step 6: Commit** `platform: locate FA via CLI/env/FAF/Steam libraries`.

### Task 8: Hook directories and retail sim boot (M178)

**Status:** ✅ DONE `4264518` (hooks live on the VFS, not LuaState — every Lua state sharing the VFS gets them), `fead351` (retail categories + first retail APIs); exit criterion met: retail SCMP_009 100 ticks, 0 Lua errors

**Files:**
- Modify: `src/lua/init_loader.{hpp,cpp}` (read the `hook` table into
  `InitConfig`/`LuaState` state), `src/lua/engine_bindings.cpp:l_doscript` (run hooks), and the
  bindings files where retail globals are missing
- Test: `tests/test_lua_state.cpp` (hook test with the existing `MemoryMount`)

**Interfaces:**
- Produces: `void osc::lua::LuaState::set_hook_dirs(std::vector<std::string>)` and
  `const std::vector<std::string>& hook_dirs() const`. `doscript(path, env)` runs `path`, then for
  each hook dir `h` in order, every VFS file `h + path` that exists, in the same env. This matches
  Moho's semantics: `/schook/lua/simInit.lua` extends `/lua/simInit.lua`.
- The hook list must be copied into every Lua state the engine creates: init, sim, UI, and
  every reload.

- [ ] **Step 1: Failing test.** Build a `MemoryMount` with `/lua/a.lua` = `x = 1` and
  `/schook/lua/a.lua` = `x = x + 10`, set the hook dirs to `{"/schook"}`, run `doscript('/lua/a.lua')`,
  and expect `x == 11`. Add a negative case: with no hook dirs, `x == 1`.
- [ ] **Step 2:** Run it and confirm it fails.
- [ ] **Step 3: Implement** in `l_doscript`. After a successful pcall, iterate the hook dirs, and
  for each `vfs->file_exists(h + path)` load and pcall it with the same env. Propagate errors the
  same way as the main chunk.
- [ ] **Step 4:** Run the tests.
- [ ] **Step 5: Retail boot loop.** Run
  `opensupcom --map /maps/SCMP_009/SCMP_009_scenario.lua --ticks 100`. That uses Task 7
  discovery; before Task 7 lands, pass `--init .../bin/SupComDataPath.lua --fa-path ...`.
  Take the first error, implement the missing engine function faithfully (not as a silent
  stub), or, if it is truly cosmetic, add it to the stub classification with a comment. Rebuild
  and repeat until the run reaches tick 100. **The first known error:**
  `CreatePrefetchSet` (simInit.lua:232).
- [ ] **Step 6:** Record the remaining Lua errors and warnings as the baseline for the M184
  report.
- [ ] **Step 7: Commit** each binding family separately:
  - `lua: doscript applies init hook directories`
  - `sim: retail boot globals (CreatePrefetchSet, …)`

### Task 9: Lua errors unwind C++ frames (M179)

**Status:** ✅ DONE `78d4893`, review follow-up `ffe832d`

**Files:**
- Modify: `third_party/lua-5.0/CMakeLists.txt` (compile the `.c` files as C++ with
  `set_source_files_properties(... LANGUAGE CXX)`), `third_party/lua-5.0/ldo.c`
  (`luaD_throw`/`luaD_rawrunprotected`: `try { f(L, ud); } catch (lua_longjmp* lj) { … }` when
  `__cplusplus`), the Lua headers' `extern "C"` blocks, and every `extern "C" { #include <lua.h> }`
  in `src/` (must stay consistent: either all C++ linkage, or keep C linkage via
  `extern "C"` on the Lua API and compile as C++)
- Test: `tests/test_lua_state.cpp`

**Interfaces:**
- Produces: `lua_error`/`luaL_error` run destructors of C++ objects in the frames they unwind,
  on every compiler.

- [ ] **Step 1: Failing test.** Register a C function that constructs a `struct Probe { ~Probe(){ ++g_destroyed; } }`
  on its stack and then calls `luaL_error`. `pcall` it from Lua and expect
  `g_destroyed == 1`. On GCC/Clang with the current longjmp build it is 0.
- [ ] **Step 2:** Run it and confirm it fails on Linux.
- [ ] **Step 3: Implement the exception-based throw.** Mirror Lua 5.1's `LUAI_THROW`/`LUAI_TRY`:
  throw a pointer to the `lua_longjmp` struct, and catch it in `luaD_rawrunprotected`. Keep the
  API's C linkage (`extern "C"` in `lua.h` when `__cplusplus`) so no call sites change.
- [ ] **Step 4:** Run the full unit suite, the ASan suite with `detect_leaks=1`, and the retail
  headless boot.
- [ ] **Step 5: Commit** `lua: build Lua 5.0 as C++ so errors unwind C++ frames`.

### Task 10: Asserting integration harness (M180)

**Status:** ✅ DONE `5d06ab3`, `2987108` — retail baseline: 44 gate / 56 retail-gap / 5 mp

**Files:**
- Modify: `src/integration_tests.{hpp,cpp}`: add `int g_failures` and `void osc::test::fail(std::string_view)`,
  counted by every `[FAIL]`/`FAILED` path. `main.cpp` returns `g_failures ? 1 : 0` from test
  modes, and `--full-smoke-test` returns non-zero when the smoke report has issues.
- Create: `tests/integration/CMakeLists.txt`, which registers
  `add_test(NAME data.<mode> COMMAND opensupcom --map ... --<mode>)` with `LABELS data` and
  `ENVIRONMENT`. It is only added when `OSC_FA_PATH` is set at configure time, or registered with
  `DISABLED` otherwise.
- Remove: `smoke_report.txt` from the repo (write it under `build/` instead, and add it to
  `.gitignore`)

**Interfaces:**
- Produces: `ctest --preset linux-debug -L data` runs the data-backed suite, and each test
  fails via its exit code.

- [ ] **Step 1:** Make one existing mode assert. Start with `--victory-test`, and have a
  deliberately broken expectation make the process exit 1. Revert the break.
- [ ] **Step 2:** Convert the log-only pass/fail sites. Grep for `[FAIL]` and `FAILED` in
  `integration_tests.cpp` and the embedded Lua (`LOG('... FAILED')`). Route the Lua ones through
  a new `__osc_test_fail(msg)` global that increments `g_failures`.
- [ ] **Step 3:** Register the 10 most valuable modes in ctest:
  - smoke
  - full-smoke
  - victory
  - economy
  - construction
  - combat
  - pathfinding
  - AI skirmish (short)
  - lobby-flow
  - MP host+join (a two-process fixture using the `FIXTURES_SETUP` pair)
- [ ] **Step 4:** Run `ctest -L data` on this machine against retail. Every test either passes
  or has a tracked issue.
- [ ] **Step 5: Commit** `test: integration modes return exit codes; ctest data label`.

### Task 11: CI (M181)

**Status:** ✅ Workflow landed `2987108`; first green run pending on PR #18

**Files:**
- Create: `.github/workflows/ci.yml`

**Jobs:**
- `linux-gcc` and `linux-clang` on `ubuntu-24.04`:
  - apt: `ninja-build`, `xorg-dev`, `libwayland-dev`, `libxkbcommon-dev`, `wayland-protocols`,
    `pkg-config`, `autoconf`, `libtool`
  - vcpkg via `lukka/run-vcpkg` or a cached `~/.cache/vcpkg`
  - `cmake --preset linux-debug`, build, then `ctest --preset linux-debug -LE data`
- `linux-asan`: the same with the `linux-asan` preset.
- `windows-msvc` on `windows-2022`: `cmake --preset default`, build, `ctest -C Debug -LE data`.

- [ ] **Step 1:** Write the workflow.
- [ ] **Step 2:** Push the branch and watch the runs with `gh run watch`. Iterate until green.
- [ ] **Step 3:** Record the CI badge and job names in `README.md`.
- [ ] **Step 4: Commit** `ci: GitHub Actions for Linux GCC/Clang/ASan and Windows MSVC`.

### Task 12: Offscreen capture and golden images (M182)

**Status:** ✅ DONE `7318880` (Linux WSI loader — the renderer had never run on Linux), `0590440` (capture, `--screenshot`, `--golden`; bit-exact run to run), `46b04ad` (spawn-height bug found by the first capture)

**Files:**
- Modify: `src/renderer/renderer.{hpp,cpp}`: add `TRANSFER_SRC` to the scene color image, and add
  `bool Renderer::capture_scene(std::vector<uint8_t>& rgba8, uint32_t& w, uint32_t& h)`, which
  copies to a host-visible buffer after the frame fence and converts RGBA16F→RGBA8.
- Modify: `src/main.cpp`: `--screenshot <file.png> [--screenshot-frame N]` exits after writing.
- Create: `src/renderer/image_write.cpp` (stb_image_write PNG; stb is already vendored under
  `third_party/stb`), `tools/golden_compare.cpp` (the `osc_golden_compare` executable: mean
  absolute error plus the fraction of pixels whose delta exceeds a threshold)
- Test: `tests/test_image_compare.cpp` (compare math on synthetic images)

- [ ] **Step 1:** Write a failing unit test for the compare metric: identical images give 0;
  one inverted pixel in 100 gives a fraction of 0.01.
- [ ] **Step 2:** Implement the compare tool and the capture path.
- [ ] **Step 3:** Capture SCMP_009 at frame 120 with a fixed camera. Store the golden **outside
  the repo** (it contains game art), under `$OSC_GOLDEN_DIR`. Register `data.golden.scmp009` in
  ctest.
- [ ] **Step 4: Commit** `renderer: offscreen scene capture + golden image compare`.

### Task 13: Documentation and status

**Status:** ✅ DONE `2987108` + this update

**Files:**
- Modify: `README.md` (Linux build section; the status numbers must match reality),
  `docs/current-state.md` (Linux verified runs, the metrics table from ROADMAP §9, and the
  contradictory MP "still to build" text removed)

- [ ] **Step 1:** Write the Linux quick start. List the prerequisites (`git`, `curl`, `zip`,
  `unzip`, `tar`, `cmake`, `ninja`, a C++20 compiler, Vulkan driver, X11/Wayland dev headers),
  then the preset commands, then running with auto-discovered Steam FA.
- [ ] **Step 2:** Update current-state with the measured numbers from this phase.
- [ ] **Step 3: Commit** `docs: Linux build + current state`.

---

## Self-review

- **Coverage.**
  - Every M175–M182 item in the roadmap maps to a task: M175 → Tasks 1 and 4, M176 → 5 and 6,
    M177 → 7, M178 → 2, 3 and 8, M179 → 9, M180 → 10, M181 → 11, M182 → 12.
  - The docs are Task 13.
- **Placeholders.**
  - Task 8, Step 5 is intentionally iterative: the set of missing retail globals can only be
    discovered by running. It has a concrete loop, a first known item and an exit condition.
  - Task 11's apt list is the known glfw/vcpkg prerequisite set; CI iteration may add packages.
- **Types.** The `EnvLookup` signature is shared by Tasks 5 and 7, and `GameInstall` fields
  match the `InitConfig` fields (`fa_path`, `init_file`, `faf_data_path`).
