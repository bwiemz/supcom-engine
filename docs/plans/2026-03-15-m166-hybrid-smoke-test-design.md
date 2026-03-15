# M166: Hybrid Smoke Test — Design Spec

**Date:** 2026-03-15
**Goal:** Surface all blocking stubs and missing moho methods across the full game lifecycle (menu → lobby → gameplay → score → return-to-lobby) via two complementary modes: an automated flow-through test and an instrumented interactive mode.

**Motivation:** The existing `--smoke-test` only exercises 100 sim ticks + 100 UI frames in headless mode. It never drives the front-end UI, lobby, game-over, or return-to-lobby code paths. Critical gaps (AI builder methods, lobby UI bindings, score screen dependencies) remain invisible until a human manually runs the game and hits errors. This milestone closes that visibility gap.

---

## Two Modes

### Mode 1: `--full-smoke-test` (Automated Flow-Through)

A headless, non-interactive test that programmatically drives the engine through every GameState transition. Requires `--map <path>` to specify the scenario.

**Five phases, executed sequentially:**

| Phase | GameState | What it exercises | Frames/Ticks |
|-------|-----------|-------------------|--------------|
| FRONT_END | INIT → FRONT_END | `CreateUI()`, main menu Lua, LazyVar layout, font loading | 10 UI frames |
| LOBBY | FRONT_END → lobby setup | Launch config construction, FrontEndData, launch signal | 10 UI frames |
| GAME | LOADING → GAME | 19-step reload, sim boot, AI initialization, economy, construction, combat, pathfinding | 1000 sim ticks (~33s game time) |
| SCORE | GAME → SCORE | Force enemy defeat, NoteGameOver, score screen callbacks, GetArmyScore, stat display | 10 UI frames |
| RETURN | SCORE → FRONT_END | `__osc_return_to_lobby` teardown, CreateUI re-initialization | 10 UI frames |

---

### Architectural Prerequisite: Extract Reload Sequence

The 19-step reload sequence currently lives inside the `while (!renderer.should_close())` windowed game loop (main.cpp ~lines 1261-1422), which is unreachable when `headless = true`. Before implementing `--full-smoke-test`, this reload sequence must be **extracted into a standalone function**:

```cpp
// Returns true on success, false on critical failure
bool execute_reload_sequence(
    std::unique_ptr<osc::lua::LuaState>& sim_lua_state,
    std::unique_ptr<osc::sim::SimState>& sim_state,
    osc::lua::LuaState& ui_lua_state,
    osc::BlueprintStore& store,
    const std::string& scenario_path,
    const std::string& ai_personality,
    osc::Renderer* renderer  // nullptr for headless
);
```

Both the interactive game loop and `--full-smoke-test` call this function. The `renderer` parameter is nullable — when null, skip all renderer-specific steps (clear_scene, build_scene, terrain mesh, etc.) but still execute Lua VM init, blueprint reload, sim boot, and session start.

---

### Headless UI State Considerations

In headless mode (`--full-smoke-test`), the following registry pointers are absent: `__osc_renderer`, `__osc_input_handler`, `__osc_factory_queue`. CreateUI and other UI Lua code may call moho bindings that depend on these. This is **acceptable** — the harness will record them as MISSING_METHOD or PCALL_ERROR entries. The smoke test is diagnostic; it doesn't need a working renderer to discover what the UI Lua code *tries to call*.

Lightweight nil-guards in moho bindings that already check for null pointers (e.g., `if (!renderer) return 0;`) will silently succeed. This is fine — it means those methods are already headless-safe.

---

### Phase Execution Details

**Shared helper — `pump_ui_frames(lua_State* uL, int count)`:**

Each UI frame pump executes the same per-frame work as the interactive game loop:
1. `ui_thread_manager.resume_all(uL)` — resume UI coroutines
2. `call_on_beat(uL, dt)` — fire OnBeat Lua callback (dt = 1/30)
3. `beat_registry.fire_all(uL)` — fire all registered beat functions
4. SimCallback queue drain (if sim_state exists) — processes UI→sim commands

This helper is called in every phase with the appropriate frame count.

**Phase 1 — FRONT_END:**
1. SmokeTestHarness interceptors installed on both sim_L and ui_L
2. Destroy sim_state if it exists (FRONT_END state has no active sim, matching the real interactive flow where sim_state is null during lobby)
3. Transition GameStateManager to FRONT_END
4. Call `CreateUI()` on ui_L (loads FA's `/lua/ui/menus/main.lua`)
5. `pump_ui_frames(uL, 10)`
6. Harness records all missing globals/methods/pcall errors with phase tag "FRONT_END"

**Phase 2 — LOBBY:**
1. Set harness phase to "LOBBY"
2. Programmatically build the launch configuration using the real launch mechanism:
   - Create a Lua table on ui_L with `ScenarioFile = map_path`
   - Configure two armies: human (index 0) + AI (index 1, personality from `--ai-personality` flag, default "adaptive")
   - Store the scenario path in `__osc_launch_scenario` registry key
   - Store session config in `__osc_front_end_data` registry key (FrontEndData)
   - Set `__osc_launch_requested = true` in the registry
3. `pump_ui_frames(uL, 10)` to let any lobby coroutines settle
4. Harness records issues tagged "LOBBY"

**Phase 3 — GAME:**
1. Set harness phase to "GAME"
2. Call `execute_reload_sequence()` with `renderer = nullptr` (headless)
   - On critical failure (returns false): log error, skip all remaining phases, proceed directly to final report output
3. Fire `call_on_first_update(uL)` once (matches interactive flow, triggers initial UI layout)
4. Run 1000 sim ticks:
   - Each tick: `sim_state->tick()`
   - Every 10 ticks: `pump_ui_frames(uL, 1)` (interleaved UI processing)
   - Every 250 ticks: log progress (entity count, army stats)
5. Harness records issues tagged "GAME"

**Phase 4 — SCORE:**
1. Set harness phase to "SCORE"
2. Force all non-player, non-civilian armies to Defeat state
3. Verify `player_result()` returns Victory (1)
4. Call `NoteGameOver(uL)` — fires the Lua callback that the interactive loop uses to initialize the score screen
5. Transition GameStateManager to SCORE
6. `pump_ui_frames(uL, 10)` for score screen layout
7. Harness records issues tagged "SCORE"

**Phase 5 — RETURN:**
1. Set harness phase to "RETURN"
2. Set `__osc_return_to_lobby` registry flag
3. Destroy sim_state (matching the interactive return-to-lobby teardown)
4. Transition to FRONT_END
5. Call `CreateUI()` on ui_L
6. `pump_ui_frames(uL, 10)`
7. Harness records issues tagged "RETURN"

**Exit:** Print consolidated report, write to `smoke_report.txt`, exit with code 0 (diagnostic, not pass/fail).

---

### Mode 2: `--instrument` (Interactive Instrumented Mode)

Silent harness overlay on normal interactive play.

1. At startup, install SmokeTestHarness interceptors on both Lua VMs
2. Game runs normally in windowed mode — user clicks through menus, plays the game
3. All missing globals/methods/pcall errors are silently accumulated (no phase tags — flat collection)
4. On window close / `ExitApplication()`:
   - Print report to console
   - Write report to `smoke_report.txt` in working directory
5. No other behavioral changes — same game loop, same rendering, same input

---

## Report Format

Both modes produce the same structured report:

```
=== SMOKE TEST REPORT ===
Phases exercised: FRONT_END, LOBBY, GAME (1000 ticks), SCORE, RETURN

--- Phase: FRONT_END (10 UI frames) ---
  [MISSING_METHOD]  UIControl.SetNeedsFrameUpdate      x12  (first: /lua/ui/controls/control.lua:45)
  [PCALL_ERROR]     attempt to index nil value           x1   (first: /lua/ui/menus/main.lua:230)

--- Phase: LOBBY (10 UI frames) ---
  [MISSING_GLOBAL]  MapUtil.LoadScenario                 x3   (first: /lua/ui/maputil.lua:12)

--- Phase: GAME (1000 sim ticks) ---
  [MISSING_METHOD]  ArmyBrain.PBMAddBuildLocation        x47  (first: /lua/ai/aibuildstructures.lua:88)
  [MISSING_METHOD]  ArmyBrain.CheckBlockingTerrain       x15  (first: /lua/ai/aiattackutilities.lua:201)

--- Phase: SCORE (10 UI frames) ---
  (clean)

--- Phase: RETURN (10 UI frames) ---
  (clean)

=== SUMMARY ===
Total: 4 unique issues, 78 total occurrences
  MISSING_METHOD:  3 unique (74 hits)
  MISSING_GLOBAL:  1 unique (3 hits)
  PCALL_ERROR:     1 unique (1 hit)
```

For `--instrument` mode, phase headers are omitted — issues are grouped by category only.

**File output:** `smoke_report.txt` in the current working directory (overwritten each run).

---

## Changes to SmokeTestHarness

The existing `SmokeTestHarness` class (smoke_test.hpp/cpp) needs minor extensions:

1. **Phase tracking:** Add `set_phase(const std::string& phase)` method. Each recorded issue stores the active phase at time of recording.
2. **File output:** Add `write_report_to_file(const std::string& path)` method that writes the same formatted report to disk.
3. **Phase-grouped printing:** `print_report()` gains a `bool group_by_phase` parameter. `--full-smoke-test` passes `true`, `--instrument` and legacy `--smoke-test` pass `false`.

**EntryKey change:** The `EntryKey` struct becomes `{category, name, phase}` — the same missing method in GAME (47 hits) and LOBBY (3 hits) produces two separate report lines.

**Backward compatibility:** The existing `--smoke-test` mode never calls `set_phase()`, so the phase field defaults to `""` (empty string). It calls `print_report(false)` for flat output, preserving its current behavior exactly. The EntryKey `{category, name, ""}` is a single bucket for all hits — identical to the old behavior.

**Error routing:** Add a static `SmokeTestHarness* active_instance()` accessor. When the harness is active, `call_lua_global()` and other pcall wrappers check for the active harness and route errors to it in addition to spdlog. This ensures pcall errors from all code paths appear in the report, not just spdlog.

**Per-phase error cap:** If a single phase accumulates more than 500 unique issues, further issues in that phase are silently dropped and a truncation notice is added to the report. This prevents cascading failures (e.g., a failed reload step producing thousands of downstream errors) from making the report unusable.

---

## Changes to main.cpp

**New flags:**
- `--full-smoke-test` — triggers the 5-phase automated flow. Sets `headless = true`. Mutually exclusive with other test flags.
- `--instrument` — enables harness during interactive mode. Does NOT set headless. Compatible with normal play (no `--map` required for front-end exploration, `--map` optional for direct game launch).

**Structural change:** Extract the 19-step reload sequence into `execute_reload_sequence()` (see Architectural Prerequisite above). Both the interactive game loop and `--full-smoke-test` call this function.

**Estimated new code:** ~200 lines in main.cpp for the flow driver + pump_ui_frames helper, ~50 lines in smoke_test.hpp/cpp for phase tracking, file output, error routing, and error cap.

**No changes to:** renderer, sim, UI framework, moho_bindings, or any other subsystem. The smoke test is purely observational.

---

## Integration with Existing Tests

The `--full-smoke-test` flag sets `headless = true` (no renderer window). It is mutually exclusive with `--smoke-test`, `--stress-test`, `--ai-skirmish`, etc. — only one test mode at a time.

The `--instrument` flag does NOT set headless — it runs alongside the normal interactive game loop.

The existing `--smoke-test` is unchanged in behavior. It continues to call `print_report(false)` for flat output.

---

## Success Criteria

- `--full-smoke-test` completes all 5 phases without crashing
- Report accurately identifies every missing method/global hit during the flow
- Report file (`smoke_report.txt`) is written and readable
- `--instrument` mode produces the same quality report from manual play
- No regressions in existing `--smoke-test` or unit tests
- Reload failure in Phase 3 does not crash the test (graceful skip to report)

---

## Roadmap Context

This is M166 in the following sequence:

| Milestone | Task | Depends On |
|-----------|------|------------|
| **M166** | Hybrid smoke test | — |
| **M167** | AI builder manager (PBMAddBuildLocation, etc.) | M166 report identifies gaps |
| **M168** | Fix blocking stubs from smoke test report | M166 report |
| **M169** | Loading screen overlay | M168 |
| **M170** | Map preview in lobby | M168 |
| **M171** | Sound completeness audit | M168 |
| **M172** | Main menu background fallback | M168 |
