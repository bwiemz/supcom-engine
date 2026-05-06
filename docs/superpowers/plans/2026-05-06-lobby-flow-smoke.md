# Lobby Flow Smoke Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove the skirmish lifecycle can launch through the lobby communication API, not only through a manually built `LaunchSinglePlayerSession` call.

**Architecture:** Keep the existing UI Lua VM and smoke harness. Add focused unit coverage for `lobby:LaunchGame(config)` preserving real lobby config into `FrontEndData` and launch registry state. Then update the executable full-smoke lobby phase to create an `InternalCreateLobby` instance and launch through `lobby:LaunchGame(config)`, while preserving the existing reload/game/score/return phases.

**Tech Stack:** C++20, Lua 5.0 C API, Catch2, existing Visual Studio CMake build.

---

### Task 1: Unit Regression For Lobby Launch Handoff

**Files:**
- Modify: `tests/test_lua_state.cpp`

- [x] **Step 1: Write the failing test**

Add a Catch2 test that registers moho/UI bindings, attaches `FrontEndData`, constructs a lobby class from `moho.lobby_methods`, calls `lobby:LaunchGame(config)`, and asserts:
- `__osc_launch_requested` is true.
- `__osc_launch_scenario` is the selected map.
- `FrontEndData.sessionConfig.PlayerOptions[2].AIPersonality` survives the handoff.
- The config is normalized with top-level `ScenarioFile`.

- [x] **Step 2: Run test to verify RED**

Run:
`build\tests\Debug\osc_tests.exe "Lobby LaunchGame preserves lobby config for skirmish launch"`

Expected before implementation: FAIL because the test should expose any missing launch handoff behavior.

- [x] **Step 3: Implement the minimal binding fix if RED exposes one**

Modify only `src/lua/moho_bindings.cpp` if needed. The expected path is `lobby_LaunchGame()` delegating to `LaunchSinglePlayerSession(config)`.

- [x] **Step 4: Run test to verify GREEN**

Run:
`build\tests\Debug\osc_tests.exe "Lobby LaunchGame preserves lobby config for skirmish launch"`

Expected: PASS.

### Task 2: Exercise Lobby Launch API In Full Smoke

**Files:**
- Modify: `src/main.cpp`

- [x] **Step 1: Update the full-smoke lobby phase**

Replace the direct `LaunchSinglePlayerSession(config)` call in Phase 2 with:
- `InternalCreateLobby` using a class copied from `moho.lobby_methods`.
- A config table equivalent to the current smoke config.
- `lobby:LaunchGame(config)`.

- [x] **Step 2: Run executable smoke verification**

Run:
`build\Debug\opensupcom.exe --full-smoke-test --map "/maps/SCMP_009/SCMP_009_scenario.lua"`

Expected: smoke report has `0 unique issues, 0 total occurrences`, and logs show the full lifecycle reaches score and return.

### Task 3: Final Verification

**Files:**
- No new files beyond Task 1 and Task 2.

- [x] **Step 1: Build tests and app**

Run:
`cmake --build build --config Debug --target osc_tests`

Run:
`cmake --build build --config Debug --target opensupcom`

Expected: both builds exit 0.

- [x] **Step 2: Run full unit suite**

Run:
`build\tests\Debug\osc_tests.exe`

Expected: all tests pass.

- [x] **Step 3: Confirm smoke report**

Run:
`Get-Content smoke_report.txt`

Expected:
`=== Smoke Test Report (0 unique issues, 0 total occurrences) ===`
