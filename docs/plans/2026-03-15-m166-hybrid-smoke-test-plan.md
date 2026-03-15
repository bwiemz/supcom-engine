# M166: Hybrid Smoke Test — Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `--full-smoke-test` (automated 5-phase lifecycle flow) and `--instrument` (silent harness during interactive play) modes that surface all missing moho methods and Lua errors across the complete game lifecycle.

**Architecture:** Extend SmokeTestHarness with phase tracking, file output, error routing, and per-phase error cap. Extract the 19-step reload sequence from the windowed game loop into a reusable function. Add a `pump_ui_frames()` helper for consistent UI frame processing. Wire both modes into main.cpp's flag-dispatch system.

**Tech Stack:** C++17, Lua 5.0, spdlog, Catch2

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `src/lua/smoke_test.hpp` | Modify | Add phase tracking, file output, error routing, error cap to SmokeTestHarness |
| `src/lua/smoke_test.cpp` | Modify | Implement new methods: set_phase, write_report_to_file, print_report(bool), active_instance |
| `src/main.cpp` | Modify | Extract reload sequence, add pump_ui_frames, add --full-smoke-test and --instrument flows |
| `tests/test_smoke_harness.cpp` | Create | Unit tests for phase tracking, error cap, report grouping |
| `tests/CMakeLists.txt` | Modify | Add test_smoke_harness.cpp to test sources |

---

## Chunk 1: SmokeTestHarness Extensions

### Task 1: Add Phase Tracking to SmokeTestHarness

Extend EntryKey to include phase, add set_phase() method, update record() to stamp current phase.

**Files:**
- Modify: `src/lua/smoke_test.hpp` (lines 60-77)
- Modify: `src/lua/smoke_test.cpp` (lines 14-49)

- [ ] **Step 1: Write failing test for phase tracking**

Create `tests/test_smoke_harness.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "lua/smoke_test.hpp"

TEST_CASE("SmokeTestHarness phase tracking", "[smoke]") {
    osc::lua::SmokeTestHarness harness;

    SECTION("entries record current phase") {
        harness.set_phase("GAME");
        harness.record(osc::lua::SmokeCategory::MissingMethod,
                       "Brain.PBMAddBuildLocation", "ai.lua:10");
        harness.record(osc::lua::SmokeCategory::MissingMethod,
                       "Brain.PBMAddBuildLocation", "ai.lua:20");

        harness.set_phase("LOBBY");
        harness.record(osc::lua::SmokeCategory::MissingMethod,
                       "Brain.PBMAddBuildLocation", "lobby.lua:5");

        auto report = harness.generate_report();
        // Same method in two phases = two entries
        REQUIRE(report.size() == 2);

        // Find the GAME entry
        bool found_game = false, found_lobby = false;
        for (const auto& e : report) {
            if (e.phase == "GAME") {
                REQUIRE(e.count == 2);
                REQUIRE(e.first_location == "ai.lua:10");
                found_game = true;
            }
            if (e.phase == "LOBBY") {
                REQUIRE(e.count == 1);
                found_lobby = true;
            }
        }
        REQUIRE(found_game);
        REQUIRE(found_lobby);
    }

    SECTION("default phase is empty string") {
        harness.record(osc::lua::SmokeCategory::MissingGlobal,
                       "SomeGlobal", "test.lua:1");
        auto report = harness.generate_report();
        REQUIRE(report.size() == 1);
        REQUIRE(report[0].phase.empty());
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe "[smoke]"`
Expected: FAIL — `SmokeReportEntry` has no `phase` field, `set_phase` doesn't exist.

- [ ] **Step 3: Update SmokeTestHarness header**

In `src/lua/smoke_test.hpp`, make these changes:

Add `phase` field to `SmokeReportEntry` (line ~23):
```cpp
struct SmokeReportEntry {
    SmokeCategory category;
    std::string name;
    std::string first_location;
    std::string phase;
    u32 count;
};
```

Add `phase` to `EntryKey` (line ~60):
```cpp
struct EntryKey {
    SmokeCategory category;
    std::string name;
    std::string phase;
    bool operator==(const EntryKey& o) const {
        return category == o.category && name == o.name && phase == o.phase;
    }
};
```

Update `EntryKeyHash` (line ~67):
```cpp
struct EntryKeyHash {
    std::size_t operator()(const EntryKey& k) const {
        auto h1 = std::hash<int>{}(static_cast<int>(k.category));
        auto h2 = std::hash<std::string>{}(k.name);
        auto h3 = std::hash<std::string>{}(k.phase);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};
```

Add public method and private member:
```cpp
// Public:
void set_phase(const std::string& phase) { current_phase_ = phase; }

// Private:
std::string current_phase_;
```

- [ ] **Step 4: Update record() and generate_report()**

In `src/lua/smoke_test.cpp`:

Update `record()` (line ~14) to use current_phase_ in the key:
```cpp
void SmokeTestHarness::record(SmokeCategory category,
                               const std::string& name,
                               const std::string& location) {
    EntryKey key{category, name, current_phase_};
    auto& entry = entries_[key];
    if (entry.count == 0) {
        entry.first_location = location;
    }
    entry.count++;
}
```

Update `generate_report()` (line ~22) to include phase in report entries:
```cpp
std::vector<SmokeReportEntry> SmokeTestHarness::generate_report() const {
    std::vector<SmokeReportEntry> result;
    result.reserve(entries_.size());
    for (const auto& [key, data] : entries_) {
        result.push_back({key.category, key.name, data.first_location,
                          key.phase, data.count});
    }
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.count > b.count; });
    return result;
}
```

- [ ] **Step 5: Add test file to CMakeLists.txt**

In `tests/CMakeLists.txt`, add `test_smoke_harness.cpp` to the test sources list (next to existing test files).

- [ ] **Step 6: Run test to verify it passes**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe "[smoke]"`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
git add src/lua/smoke_test.hpp src/lua/smoke_test.cpp tests/test_smoke_harness.cpp tests/CMakeLists.txt
git commit -m "Add phase tracking to SmokeTestHarness (EntryKey includes phase)"
```

---

### Task 2: Phase-Grouped Report Printing and File Output

Add `print_report(bool group_by_phase)` and `write_report_to_file()`.

**Files:**
- Modify: `src/lua/smoke_test.hpp` (print_report signature)
- Modify: `src/lua/smoke_test.cpp` (print_report + write_report_to_file)
- Modify: `tests/test_smoke_harness.cpp`

- [ ] **Step 1: Write test for file output**

In `tests/test_smoke_harness.cpp`, add:

```cpp
#include <fstream>
#include <filesystem>

TEST_CASE("SmokeTestHarness file output", "[smoke]") {
    osc::lua::SmokeTestHarness harness;
    harness.set_phase("GAME");
    harness.record(osc::lua::SmokeCategory::MissingMethod,
                   "Brain.Foo", "test.lua:1");

    auto path = std::filesystem::temp_directory_path() / "test_smoke_report.txt";
    harness.write_report_to_file(path.string());

    std::ifstream in(path);
    REQUIRE(in.good());
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    REQUIRE(content.find("MISSING_METHOD") != std::string::npos);
    REQUIRE(content.find("Brain.Foo") != std::string::npos);
    REQUIRE(content.find("GAME") != std::string::npos);

    std::filesystem::remove(path);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe "[smoke]"`
Expected: FAIL — `write_report_to_file` doesn't exist.

- [ ] **Step 3: Update header**

In `src/lua/smoke_test.hpp`, change `print_report` signature and add new method:
```cpp
void print_report(bool group_by_phase = false) const;
void write_report_to_file(const std::string& path, bool group_by_phase = true) const;
```

- [ ] **Step 4: Implement print_report(bool) and write_report_to_file()**

In `src/lua/smoke_test.cpp`, replace the existing `print_report()` (line ~40) with:

```cpp
void SmokeTestHarness::print_report(bool group_by_phase) const {
    static const char* cat_names[] = {
        "MISSING_GLOBAL", "MISSING_METHOD", "PCALL_ERROR", "WRONG_RETURN"};

    auto report = generate_report();
    u32 total = total_count();
    spdlog::info("=== SMOKE TEST REPORT ({} unique issues, {} total occurrences) ===",
                 report.size(), total);

    if (group_by_phase) {
        // Collect phases in order of first appearance
        std::vector<std::string> phases;
        std::unordered_set<std::string> seen;
        for (const auto& e : report) {
            if (seen.insert(e.phase).second) {
                phases.push_back(e.phase);
            }
        }

        for (const auto& phase : phases) {
            spdlog::info("--- Phase: {} ---", phase.empty() ? "(default)" : phase);
            bool any = false;
            for (const auto& e : report) {
                if (e.phase != phase) continue;
                any = true;
                spdlog::info("  [{:<14s}]  {:<40s}  x{:<4d}  (first: {})",
                             cat_names[static_cast<int>(e.category)],
                             e.name, e.count, e.first_location);
            }
            if (!any) spdlog::info("  (clean)");
        }
    } else {
        for (const auto& e : report) {
            spdlog::info("  [{:<14s}]  {:<40s}  x{:<4d}  (first: {})",
                         cat_names[static_cast<int>(e.category)],
                         e.name, e.count, e.first_location);
        }
    }
}

void SmokeTestHarness::write_report_to_file(const std::string& path,
                                             bool group_by_phase) const {
    static const char* cat_names[] = {
        "MISSING_GLOBAL", "MISSING_METHOD", "PCALL_ERROR", "WRONG_RETURN"};

    auto report = generate_report();
    std::ofstream out(path);
    if (!out) {
        spdlog::warn("Failed to write smoke report to {}", path);
        return;
    }

    out << "=== SMOKE TEST REPORT (" << report.size() << " unique issues, "
        << total_count() << " total occurrences) ===\n\n";

    if (group_by_phase) {
        // Group by phase (for --full-smoke-test)
        std::vector<std::string> phases;
        std::unordered_set<std::string> seen;
        for (const auto& e : report) {
            if (seen.insert(e.phase).second) phases.push_back(e.phase);
        }
        for (const auto& phase : phases) {
            out << "--- Phase: " << (phase.empty() ? "(default)" : phase) << " ---\n";
            bool any = false;
            for (const auto& e : report) {
                if (e.phase != phase) continue;
                any = true;
                out << "  [" << cat_names[static_cast<int>(e.category)] << "]  "
                    << e.name << "  x" << e.count
                    << "  (first: " << e.first_location << ")\n";
            }
            if (!any) out << "  (clean)\n";
            out << "\n";
        }
    } else {
        // Flat output (for --instrument)
        for (const auto& e : report) {
            out << "  [" << cat_names[static_cast<int>(e.category)] << "]  "
                << e.name << "  x" << e.count
                << "  (first: " << e.first_location << ")\n";
        }
        out << "\n";
    }

    // Summary
    u32 counts[4] = {};
    u32 uniques[4] = {};
    for (const auto& e : report) {
        int ci = static_cast<int>(e.category);
        counts[ci] += e.count;
        uniques[ci]++;
    }
    out << "=== SUMMARY ===\n";
    out << "Total: " << report.size() << " unique issues, "
        << total_count() << " total occurrences\n";
    for (int i = 0; i < 4; i++) {
        if (uniques[i] > 0)
            out << "  " << cat_names[i] << ": " << uniques[i]
                << " unique (" << counts[i] << " hits)\n";
    }
}
```

Add `#include <fstream>` and `#include <unordered_set>` at the top of smoke_test.cpp.

- [ ] **Step 5: Run tests**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe "[smoke]"`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add src/lua/smoke_test.hpp src/lua/smoke_test.cpp tests/test_smoke_harness.cpp
git commit -m "Add phase-grouped report printing and file output to SmokeTestHarness"
```

---

### Task 3: Active Instance Accessor and Per-Phase Error Cap

Add `active_instance()` static accessor for error routing, and a 500-unique-issue-per-phase cap.

**Files:**
- Modify: `src/lua/smoke_test.hpp`
- Modify: `src/lua/smoke_test.cpp`
- Modify: `tests/test_smoke_harness.cpp`

- [ ] **Step 1: Write test for error cap**

In `tests/test_smoke_harness.cpp`, add:

```cpp
TEST_CASE("SmokeTestHarness error cap", "[smoke]") {
    osc::lua::SmokeTestHarness harness;
    harness.set_phase("GAME");

    // Record 501 unique methods — only first 500 should be kept
    for (int i = 0; i < 501; i++) {
        harness.record(osc::lua::SmokeCategory::MissingMethod,
                       "Method_" + std::to_string(i), "test.lua:1");
    }

    auto report = harness.generate_report();
    int game_count = 0;
    for (const auto& e : report) {
        if (e.phase == "GAME") game_count++;
    }
    REQUIRE(game_count == 500);
}

TEST_CASE("SmokeTestHarness active instance", "[smoke]") {
    REQUIRE(osc::lua::SmokeTestHarness::active_instance() == nullptr);
    {
        osc::lua::SmokeTestHarness harness;
        harness.activate();
        REQUIRE(osc::lua::SmokeTestHarness::active_instance() == &harness);
    }
    // Destructor clears active instance
    REQUIRE(osc::lua::SmokeTestHarness::active_instance() == nullptr);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe "[smoke]"`
Expected: FAIL — `activate()`, `active_instance()` don't exist, no error cap.

- [ ] **Step 3: Add active_instance and error cap to header**

In `src/lua/smoke_test.hpp`, add to public section:

```cpp
~SmokeTestHarness() { deactivate(); }
void activate() { s_active_ = this; }
void deactivate() { if (s_active_ == this) s_active_ = nullptr; }
static SmokeTestHarness* active_instance() { return s_active_; }

static constexpr u32 MAX_ISSUES_PER_PHASE = 500;
```

Add to private section:
```cpp
static SmokeTestHarness* s_active_;
std::unordered_map<std::string, u32> phase_unique_counts_;
```

- [ ] **Step 4: Implement in cpp**

In `src/lua/smoke_test.cpp`, add at file scope (inside namespace):
```cpp
SmokeTestHarness* SmokeTestHarness::s_active_ = nullptr;
```

Update `record()` to enforce the cap:
```cpp
void SmokeTestHarness::record(SmokeCategory category,
                               const std::string& name,
                               const std::string& location) {
    EntryKey key{category, name, current_phase_};
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        // Existing entry — just increment count (no new unique)
        it->second.count++;
        return;
    }
    // New unique entry — check phase cap
    u32& phase_count = phase_unique_counts_[current_phase_];
    if (phase_count >= MAX_ISSUES_PER_PHASE) {
        return; // Silently drop
    }
    phase_count++;
    auto& entry = entries_[key];
    entry.first_location = location;
    entry.count = 1;
}
```

- [ ] **Step 5: Run tests**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe "[smoke]"`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add src/lua/smoke_test.hpp src/lua/smoke_test.cpp tests/test_smoke_harness.cpp
git commit -m "Add active_instance accessor and 500-per-phase error cap to SmokeTestHarness"
```

---

### Task 4: Route pcall Errors to Active Harness

Wire `call_lua_global()` errors to the active harness so Lua errors from all code paths appear in the report.

**Files:**
- Modify: `src/core/game_state.hpp` (line ~47, call_lua_global)
- Modify: `src/core/game_state.cpp` (if call_lua_global is defined there instead)

- [ ] **Step 1: Update call_lua_global to route errors to harness**

In `src/core/game_state.hpp`, the `call_lua_global` function (line ~47) is an inline helper. Add harness routing:

```cpp
#include "lua/smoke_test.hpp"

inline void call_lua_global(lua_State* L, const char* name) {
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_isfunction(L, -1)) {
        if (lua_pcall(L, 0, 0, 0) != 0) {
            std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
            spdlog::warn("{} error: {}", name, err);
            // Route to smoke harness if active
            auto* harness = osc::lua::SmokeTestHarness::active_instance();
            if (harness) {
                harness->record(osc::lua::SmokeCategory::PcallError, err, name);
            }
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
}
```

Apply the same pattern to `call_on_beat` (which takes a dt argument):
```cpp
inline void call_on_beat(lua_State* L, f64 dt) {
    lua_pushstring(L, "OnBeat");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_isfunction(L, -1)) {
        lua_pushnumber(L, dt);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
            spdlog::warn("OnBeat error: {}", err);
            auto* harness = osc::lua::SmokeTestHarness::active_instance();
            if (harness) {
                harness->record(osc::lua::SmokeCategory::PcallError, err, "OnBeat");
            }
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
}
```

- [ ] **Step 2: Build and verify**

Run: `cmake --build build --config Debug`
Expected: Clean compile. No test changes needed — this is purely wiring.

- [ ] **Step 3: Run existing tests for regression**

Run: `./build/tests/Debug/osc_tests.exe`
Expected: All 92+ tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/core/game_state.hpp
git commit -m "Route call_lua_global/call_on_beat pcall errors to active SmokeTestHarness"
```

---

## Chunk 2: Reload Extraction and UI Frame Pump

### Task 5: Extract Reload Sequence into Standalone Function

Move the 19-step reload sequence from the windowed game loop into a callable function so both interactive mode and `--full-smoke-test` can use it.

**Files:**
- Modify: `src/main.cpp` (lines ~1261-1422)

- [ ] **Step 1: Define the function signature above main()**

In `src/main.cpp`, before the `main()` function, add the function declaration:

```cpp
/// Execute the full reload sequence: destroy old sim, create fresh Lua VM,
/// reload blueprints, boot sim, start session.
/// Returns true on success. renderer may be nullptr for headless mode.
static bool execute_reload_sequence(
    std::unique_ptr<osc::lua::LuaState>& sim_lua_state,
    std::unique_ptr<osc::sim::SimState>& sim_state,
    osc::lua::LuaState& ui_lua_state,
    osc::BlueprintStore& store,
    osc::BlueprintStore& ui_store,
    osc::vfs::VFS& vfs,
    osc::lua::InitLoader& loader,
    osc::lua::InitConfig& config,
    osc::sim::ThreadManager& ui_thread_manager,
    osc::lua::BeatFunctionRegistry& beat_registry,
    osc::core::GameStateManager& game_state_mgr,
    osc::audio::SoundManager* sound_mgr,
    osc::Renderer* renderer,
    const std::string& scenario_path,
    const std::string& ai_personality,
    std::vector<osc::u32>& prev_selection,
    osc::ScenarioMeta& scenario_meta,
    f64& sim_accumulator);
```

**Note:** `loader` and `config` are needed for `loader.execute_init()` (step 5 of reload). `scenario_meta` is read/written during scenario loading. `sim_accumulator` is reset to 0.0 at the end of reload.

- [ ] **Step 2: Implement the function**

Extract lines ~1261-1422 from the windowed game loop into this function body. The key changes:
- All references to local variables (`sim_lua_state`, `sim_state`, `renderer`, etc.) become parameters
- Guard all renderer calls with `if (renderer)`:
  - `renderer->clear_scene()` — skip if null
  - `renderer->build_scene()` — skip if null
  - Camera reset — skip if null
- Guard `input_handler` access — not available in headless, wrap with `if (renderer)`
- The function returns `false` if any critical step fails (Lua state creation, blueprint reload, sim boot), `true` on success

Critical pattern for each fallible step:
```cpp
if (!sim_lua_state) {
    spdlog::error("Reload: failed to create sim Lua state");
    return false;
}
```

- [ ] **Step 3: Replace inline reload in game loop**

In the windowed game loop (where `__osc_launch_requested` is detected, line ~1238), replace the inline 19-step sequence with:

```cpp
bool ok = execute_reload_sequence(
    sim_lua_state, sim_state, ui_lua_state, store, ui_store, vfs,
    loader, config, ui_thread_manager, beat_registry, game_state_mgr,
    sound_mgr.get(), &renderer, scenario_path, ai_personality,
    prev_selection, scenario_meta, sim_accumulator);
if (!ok) {
    spdlog::error("Reload sequence failed");
}
```

- [ ] **Step 4: Build and run all tests**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe`
Expected: All tests pass. The extraction is a pure refactor — no behavioral change.

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "Extract 19-step reload sequence into execute_reload_sequence() function"
```

---

### Task 6: Add pump_ui_frames Helper

Create a reusable helper for pumping UI frames (coroutines, OnBeat, beat functions).

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Define pump_ui_frames above main()**

```cpp
/// Pump N UI frames: resume coroutines, fire OnBeat, fire beat functions,
/// drain SimCallback queue (if sim is active).
static void pump_ui_frames(
    osc::lua::LuaState& ui_lua_state,
    osc::sim::ThreadManager& ui_thread_manager,
    osc::lua::BeatFunctionRegistry& beat_registry,
    osc::SimCallbackQueue* sim_callback_queue,
    osc::lua::LuaState* sim_lua_state_ptr,
    int count,
    osc::u32& ui_frame_counter) {
    lua_State* uL = ui_lua_state.raw();
    for (int i = 0; i < count; i++) {
        ui_frame_counter++;
        ui_thread_manager.resume_all(ui_frame_counter);
        osc::core::call_on_beat(uL, 1.0 / 30.0);
        beat_registry.fire_all(uL);
        // Drain SimCallback queue (UI→sim commands like build orders)
        if (sim_callback_queue && sim_lua_state_ptr &&
            !sim_callback_queue->empty()) {
            auto callbacks = sim_callback_queue->drain();
            // Process via SimCallbacks.lua DoCallback (same pattern as game loop)
            // Errors will be caught by active harness via pcall routing
            for (const auto& cb : callbacks) {
                cb.execute(sim_lua_state_ptr->raw());
            }
        }
    }
}
```

- [ ] **Step 2: Build**

Run: `cmake --build build --config Debug`
Expected: Clean compile.

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "Add pump_ui_frames helper for consistent UI frame processing"
```

---

## Chunk 3: --full-smoke-test Flow Driver

### Task 7: Add --full-smoke-test Flag and 5-Phase Flow

Wire the new flag and implement the automated lifecycle test.

**Files:**
- Modify: `src/main.cpp` (flag parsing + flow driver block)

- [ ] **Step 1: Add flag parsing**

In `src/main.cpp` at the flag parsing section (line ~478), add:

```cpp
bool full_smoke_test = parse_flag(argc, argv, "--full-smoke-test");
```

In the `any_test` boolean (line ~488), add `full_smoke_test` to the OR chain.

- [ ] **Step 2: Add the full smoke test flow driver**

After the existing `--stress-test` block (line ~1670) and before the integration test dispatcher, add:

```cpp
// === Full Smoke Test (5-phase lifecycle) ===
if (full_smoke_test && !map_path.empty()) {
    osc::lua::SmokeTestHarness harness;
    harness.activate();
    osc::u32 ui_frame_counter = 0;

    // Install interceptors on ui_L (persistent across all phases).
    // sim_L interceptors installed after each VM recreation since reload destroys the old one.
    harness.install_panic_handler(ui_lua_state.raw());
    harness.install_global_interceptor(ui_lua_state.raw());
    harness.install_all_method_interceptors(ui_lua_state.raw());

    // --- Phase 1: FRONT_END ---
    spdlog::info("=== Phase 1: FRONT_END ===");
    harness.set_phase("FRONT_END");
    // Destroy sim to match real FRONT_END state (sim_state is null during lobby)
    sim_state.reset();
    sim_lua_state.reset();
    game_state_mgr.transition_to(osc::GameState::FRONT_END, ui_lua_state.raw());
    osc::core::call_lua_global(ui_lua_state.raw(), "CreateUI");
    pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry,
                   nullptr, nullptr, 10, ui_frame_counter);

    // --- Phase 2: LOBBY ---
    spdlog::info("=== Phase 2: LOBBY ===");
    harness.set_phase("LOBBY");
    {
        lua_State* uL = ui_lua_state.raw();

        // Build FrontEndData with army configuration (real mechanism)
        lua_pushstring(uL, "__osc_front_end_data");
        lua_newtable(uL);
        int fed = lua_gettop(uL);
        // ScenarioFile
        lua_pushstring(uL, "ScenarioFile");
        lua_pushstring(uL, map_path.c_str());
        lua_rawset(uL, fed);
        // PlayerCount
        lua_pushstring(uL, "PlayerCount");
        lua_pushnumber(uL, 2);
        lua_rawset(uL, fed);
        // AiPersonality
        lua_pushstring(uL, "AiPersonality");
        lua_pushstring(uL, ai_personality.c_str());
        lua_rawset(uL, fed);
        lua_rawset(uL, LUA_REGISTRYINDEX); // store table

        // Set launch scenario path
        lua_pushstring(uL, "__osc_launch_scenario");
        lua_pushstring(uL, map_path.c_str());
        lua_rawset(uL, LUA_REGISTRYINDEX);

        // Set launch requested flag
        lua_pushstring(uL, "__osc_launch_requested");
        lua_pushboolean(uL, 1);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }
    pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry,
                   nullptr, nullptr, 10, ui_frame_counter);

    // --- Phase 3: GAME ---
    spdlog::info("=== Phase 3: GAME (1000 ticks) ===");
    harness.set_phase("GAME");
    std::vector<osc::u32> smoke_selection;
    f64 smoke_sim_accumulator = 0.0;
    bool reload_ok = execute_reload_sequence(
        sim_lua_state, sim_state, ui_lua_state, store, ui_store, vfs,
        loader, config, ui_thread_manager, beat_registry, game_state_mgr,
        sound_mgr.get(), nullptr /*headless*/, map_path, ai_personality,
        smoke_selection, scenario_meta, smoke_sim_accumulator);

    if (!reload_ok) {
        spdlog::error("Phase 3: Reload failed — skipping remaining phases");
        harness.print_report(true);
        harness.write_report_to_file("smoke_report.txt");
        harness.deactivate();
        return 1;
    }

    // Re-install interceptors on fresh sim_L (old one was destroyed during reload)
    if (sim_lua_state) {
        harness.install_panic_handler(sim_lua_state->raw());
        harness.install_global_interceptor(sim_lua_state->raw());
        harness.install_all_method_interceptors(sim_lua_state->raw());
    }

    // Fire OnFirstUpdate once
    osc::core::call_on_first_update(ui_lua_state.raw());

    for (int t = 0; t < 1000; t++) {
        if (sim_state) sim_state->tick();

        // Pump UI every 10 ticks (with SimCallback draining)
        if ((t + 1) % 10 == 0) {
            pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry,
                           &sim_callback_queue, sim_lua_state.get(),
                           1, ui_frame_counter);
        }

        // Progress logging every 250 ticks
        if ((t + 1) % 250 == 0) {
            osc::i32 entity_count = 0;
            if (sim_state) {
                sim_state->entity_registry().for_each(
                    [&](const osc::sim::Entity& e) {
                        if (!e.destroyed()) entity_count++;
                    });
            }
            spdlog::info("  tick {}/1000 — {} entities", t + 1, entity_count);
        }
    }

    // --- Phase 4: SCORE ---
    spdlog::info("=== Phase 4: SCORE ===");
    harness.set_phase("SCORE");
    if (sim_state) {
        // Force all non-player, non-civilian armies to Defeat
        for (size_t i = 0; i < sim_state->army_count(); i++) {
            auto* brain = sim_state->army_at(i);
            if (brain && !brain->is_civilian() && static_cast<osc::i32>(i) != 0) {
                brain->set_state(osc::sim::BrainState::Defeat);
            }
        }
        // Verify player_result returns Victory (spec requirement)
        osc::i32 result = sim_state->player_result();
        spdlog::info("  player_result() = {} (expected 1=Victory)", result);
        osc::core::call_lua_global(ui_lua_state.raw(), "NoteGameOver");
        game_state_mgr.set_game_over(true);
        game_state_mgr.transition_to(osc::GameState::SCORE, ui_lua_state.raw());
    }
    pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry,
                   nullptr, nullptr, 10, ui_frame_counter);

    // --- Phase 5: RETURN ---
    spdlog::info("=== Phase 5: RETURN ===");
    harness.set_phase("RETURN");
    // Use the real return-to-lobby mechanism via registry flag (exercises Lua teardown path)
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_return_to_lobby");
        lua_pushboolean(uL, 1);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }
    // Execute the teardown (matching interactive flow: destroy sim, transition, CreateUI)
    sim_state.reset();
    sim_lua_state.reset();
    game_state_mgr.set_game_over(false);
    game_state_mgr.transition_to(osc::GameState::FRONT_END, ui_lua_state.raw());
    osc::core::call_lua_global(ui_lua_state.raw(), "CreateUI");
    pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry,
                   nullptr, nullptr, 10, ui_frame_counter);

    // --- Report ---
    spdlog::info("=== Full Smoke Test Complete ===");
    harness.print_report(true);
    harness.write_report_to_file("smoke_report.txt");
    spdlog::info("Report written to smoke_report.txt");
    harness.deactivate();
    return 0;
}
```

- [ ] **Step 3: Build**

Run: `cmake --build build --config Debug`
Expected: Clean compile.

- [ ] **Step 4: Run unit tests for regression**

Run: `./build/tests/Debug/osc_tests.exe`
Expected: All tests pass (no behavioral change to existing code paths).

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "Add --full-smoke-test: 5-phase automated lifecycle smoke test"
```

---

## Chunk 4: --instrument Mode and Final Validation

### Task 8: Add --instrument Flag for Interactive Instrumented Mode

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Add flag parsing**

In `src/main.cpp` at the flag parsing section, add:

```cpp
bool instrument = parse_flag(argc, argv, "--instrument");
```

**Important:** `instrument` does NOT get added to `any_test` and does NOT set `headless`. It runs alongside the normal interactive game loop.

- [ ] **Step 2: Install harness at startup when --instrument is active**

In `src/main.cpp`, after both Lua VMs are initialized and moho bindings are registered (after line ~760, before the game loop), add:

```cpp
// Instrumented mode: silent harness during interactive play
std::unique_ptr<osc::lua::SmokeTestHarness> instrument_harness;
if (instrument) {
    instrument_harness = std::make_unique<osc::lua::SmokeTestHarness>();
    instrument_harness->activate();
    // Install on ui_L (persistent). sim_L interceptors installed below and after each reload.
    instrument_harness->install_panic_handler(ui_lua_state.raw());
    instrument_harness->install_global_interceptor(ui_lua_state.raw());
    instrument_harness->install_all_method_interceptors(ui_lua_state.raw());
    if (sim_lua_state) {
        instrument_harness->install_panic_handler(sim_lua_state->raw());
        instrument_harness->install_global_interceptor(sim_lua_state->raw());
        instrument_harness->install_all_method_interceptors(sim_lua_state->raw());
    }
    spdlog::info("Instrumented mode active — smoke report on exit");
}
```

- [ ] **Step 3: Re-install harness on sim_L after reload**

Inside the `execute_reload_sequence` call site in the windowed game loop (where `__osc_launch_requested` is handled), after the reload succeeds, add:

```cpp
// Re-install instrument harness on fresh sim_L
if (instrument_harness && sim_lua_state) {
    instrument_harness->install_panic_handler(sim_lua_state->raw());
    instrument_harness->install_global_interceptor(sim_lua_state->raw());
    instrument_harness->install_all_method_interceptors(sim_lua_state->raw());
}
```

- [ ] **Step 4: Dump report on exit**

After the game loop exits (after `renderer.shutdown()`, around line ~1477), add:

```cpp
// Dump instrument report on exit
if (instrument_harness) {
    instrument_harness->print_report(false);
    instrument_harness->write_report_to_file("smoke_report.txt", false);
    spdlog::info("Instrument report written to smoke_report.txt");
    // deactivate() called by destructor via unique_ptr reset
}
```

- [ ] **Step 4: Build**

Run: `cmake --build build --config Debug`
Expected: Clean compile.

- [ ] **Step 5: Run unit tests**

Run: `./build/tests/Debug/osc_tests.exe`
Expected: All tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "Add --instrument flag: silent smoke harness during interactive play"
```

---

### Task 9: Update Existing --smoke-test for Backward Compatibility

Ensure the legacy `--smoke-test` calls `print_report(false)` to preserve flat output.

**Files:**
- Modify: `src/main.cpp` (line ~1510)

- [ ] **Step 1: Update print_report call**

In the existing `--smoke-test` block (line ~1510), change:
```cpp
harness.print_report();
```
to:
```cpp
harness.print_report(false);
```

- [ ] **Step 2: Build and test**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe`
Expected: All pass.

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "Update legacy --smoke-test to use print_report(false) for flat output"
```

---

### Task 10: Update README and Memory

**Files:**
- Modify: `README.md`
- Modify: `~/.claude/projects/c--Users-bwiem-projects-supcom-engine/memory/MEMORY.md`
- Modify: `~/.claude/projects/c--Users-bwiem-projects-supcom-engine/memory/milestones-list.md`

- [ ] **Step 1: Add new test flags to README**

In the integration test flags table in `README.md`, add:

```markdown
| `--full-smoke-test` | 5-phase lifecycle smoke test: front-end → lobby → 1000-tick game → score → return-to-lobby |
| `--instrument` | Silent smoke harness during interactive play, report on exit |
```

- [ ] **Step 2: Update milestone count in README**

Update "Over 165 milestones" to "Over 166 milestones" (or current count).

- [ ] **Step 3: Update milestones-list.md**

Add:
```markdown
- M166: Hybrid smoke test — --full-smoke-test (5-phase automated lifecycle) and --instrument (interactive harness), phase tracking, file output, error routing, per-phase error cap
```

- [ ] **Step 4: Update MEMORY.md**

Add key decisions for M166 to the appropriate section.

- [ ] **Step 5: Commit**

```bash
git add README.md
git commit -m "Add --full-smoke-test and --instrument to README, update milestone count"
```

---

## Summary

| Task | What it delivers | Files |
|------|-----------------|-------|
| 1 | Phase tracking on SmokeTestHarness | smoke_test.hpp/cpp, test |
| 2 | Phase-grouped report printing + file output | smoke_test.hpp/cpp, test |
| 3 | Active instance accessor + 500-per-phase error cap | smoke_test.hpp/cpp, test |
| 4 | Error routing from call_lua_global to harness | game_state.hpp |
| 5 | Extract reload sequence into reusable function | main.cpp |
| 6 | pump_ui_frames helper | main.cpp |
| 7 | --full-smoke-test 5-phase flow driver | main.cpp |
| 8 | --instrument interactive harness mode | main.cpp |
| 9 | Legacy --smoke-test backward compat | main.cpp |
| 10 | README + memory updates | README.md, memory files |

**Final success criteria:** `--full-smoke-test --map "/maps/SCMP_009/SCMP_009_scenario.lua"` completes all 5 phases, produces `smoke_report.txt` with actionable issue list. `--instrument` produces equivalent report from manual play on exit.
