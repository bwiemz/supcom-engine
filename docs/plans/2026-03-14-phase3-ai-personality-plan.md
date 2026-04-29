# Phase 3: AI Personality & Difficulty — Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Unblock AI initialization by increasing the instruction budget, add missing personality methods, and validate the buff/cheat pipeline end-to-end so the AI autonomously builds, produces, and attacks.

**Architecture:** The primary blocker is the 1M instruction budget — AI builder setup (`ExecutePlan` → `SetupMainBase` → `AddBuilder` → `deepcopy`) exhausts it before issuing any build orders. All Phase 2 moho methods are in place and callable. The fix is a one-line constant change in `thread_manager.hpp`, plus adding the missing `GetPlatoonSize` method to the personality table. After the budget increase, integration testing will surface any remaining missing methods or wiring issues.

**Tech Stack:** C++17, Lua 5.0 C API, Catch2, CMake + vcpkg

**Spec:** `docs/plans/2026-03-14-playable-skirmish-vs-ai-design.md` (Phase 3 section)

---

## File Structure

| File | Responsibility | Action |
|------|---------------|--------|
| `src/sim/thread_manager.hpp:23` | Instruction budget constant | Modify: 1M → 10M |
| `src/lua/moho_bindings.cpp:4764-4832` | `brain_GetPersonality` | Modify: add GetPlatoonSize closure |

No new files. Two small modifications.

## Deferred Scope (Intentionally Not Implemented)

These spec items are NOT needed for the skirmish AI to build/attack:

| Spec Item | Why Deferred |
|-----------|-------------|
| **Real personality templates** (spec 1b) — make emphasis values read from `AIPersonalityTemplate` instead of hardcoded 0.5 | FA's skirmish AIs differentiate through different builder tables in each brain Lua file (rush-ai.lua, turtle-ai.lua, etc.), NOT through personality emphasis values. The personality template system (`aipersonality.lua`) has only 2 templates (AverageJoe, Rommel) and is primarily a campaign feature. Hardcoded 0.5 (balanced) is correct for skirmish. |
| **Populating `aipersonality_methods`** (spec 1a) | `brain_GetPersonality` returns its own table with closures — `aipersonality_methods` being empty doesn't break anything. `Class(moho.aipersonality_methods)` creates an empty class, and no FA code creates `AIPersonality()` instances directly. |
| **Rush AI vs Turtle AI differentiation** (spec validation) | Requires real personality templates. Skirmish AI behavior differences come from builder tables, not personality emphasis. Deferred to Phase 4 if needed. |
| **Buff base value preservation** (spec 4) | FA's Lua buff system handles restoration through its own stacking logic (`BuffCalculate` with `Add`/`Mult` operations). The C++ side doesn't need separate `base_build_rate_` fields. Verify if issues surface during cheat testing. |
| **Lobby CheatMult/BuildMult population** (spec 5) | These come from lobby option dropdowns (`AIOpts` in `lobbyOptions.lua`). The lobby already stores them in `ScenarioInfo.Options`. For `--ai-skirmish` mode without a lobby, they default to nil (non-cheat). Only matters for cheat variants. |

---

## Chunk 1: Instruction Budget + GetPlatoonSize + Integration Test

### Task 1: Increase Instruction Budget

**Files:**
- Modify: `src/sim/thread_manager.hpp:23`

**Context:** The `ThreadManager` enforces a per-coroutine-per-resume instruction limit via `lua_sethook(LUA_MASKCOUNT)`. Currently set to 1M instructions. FA's AI builder setup chain (`ExecutePlan` → `SetupMainBase` → `AddGlobalBaseTemplate` → `AddBuilderTable` → `AddBuilder` → `Create` → `SetupBuilderConditions` → `deepcopy`) requires significantly more than 1M instructions to initialize hundreds of builder templates with recursive `deepcopy` calls. The `set_instruction_budget()` setter exists but is never called anywhere.

- [ ] **Step 1: Change the budget constant**

In `src/sim/thread_manager.hpp`, line 23, change:

```cpp
static constexpr i32 DEFAULT_INSTRUCTION_BUDGET = 1'000'000;
```

to:

```cpp
static constexpr i32 DEFAULT_INSTRUCTION_BUDGET = 10'000'000;
```

10M instructions per resume is generous enough for AI initialization while still protecting against infinite loops. The original GPG engine had no per-resume budget at all — threads ran until they yielded.

- [ ] **Step 2: Build and run tests**

Run:
```bash
cmake --build build --config Debug 2>&1 | tail -5 && ./build/tests/Debug/osc_tests.exe 2>&1 | tail -3
```
Expected: Build succeeds, all tests pass (91+ tests, 1500+ assertions). No behavioral change for existing tests since no test thread hits 1M instructions.

- [ ] **Step 3: Commit**

```bash
git add src/sim/thread_manager.hpp
git commit -m "$(cat <<'EOF'
Increase thread instruction budget from 1M to 10M

FA's AI builder setup chain (ExecutePlan → SetupMainBase →
AddBuilder → deepcopy) requires more than 1M instructions to
initialize hundreds of builder templates. 10M is generous for
init while still catching infinite loops.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Add GetPlatoonSize to Personality Table

**Files:**
- Modify: `src/lua/moho_bindings.cpp:4764-4832` (`brain_GetPersonality`)

**Context:** `brain_GetPersonality` returns a cached Lua table with 5 closures (AdjustDelay, GetAirUnitsEmphasis, GetTankUnitsEmphasis, GetBotUnitsEmphasis, GetSeaUnitsEmphasis). All return hardcoded 0.5 (balanced). Missing: `GetPlatoonSize`, called by `campaign-ai.lua:1244,1313`. Not critical for skirmish AI (only campaign), but adding it prevents nil method errors if campaign-ai code paths are ever hit. The return value should be `{1.0, 1.0}` (platoon size multiplier min/max) matching `AIPersonalityTemplate[1][4]` (AverageJoe's PlatoonSizeMult).

- [ ] **Step 1: Add GetPlatoonSize closure**

In `src/lua/moho_bindings.cpp`, inside `brain_GetPersonality`, add this block after the `GetSeaUnitsEmphasis` section (after line ~4817, before the `__index` metatable setup at line ~4819):

```cpp
    // GetPlatoonSize() → {1.0, 1.0} (platoon size multiplier {min, max})
    lua_pushstring(L, "GetPlatoonSize");
    lua_pushcfunction(L, [](lua_State* Ls) -> int {
        lua_newtable(Ls);
        lua_pushnumber(Ls, 1.0);
        lua_rawseti(Ls, -2, 1);
        lua_pushnumber(Ls, 1.0);
        lua_rawseti(Ls, -2, 2);
        return 1;
    });
    lua_rawset(L, tbl);
```

- [ ] **Step 2: Build and run tests**

Run:
```bash
cmake --build build --config Debug 2>&1 | tail -5 && ./build/tests/Debug/osc_tests.exe 2>&1 | tail -3
```
Expected: Build succeeds, all tests pass.

- [ ] **Step 3: Commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "$(cat <<'EOF'
Add GetPlatoonSize to personality table

Returns {1.0, 1.0} (balanced platoon size multiplier).
Called by campaign-ai.lua for platoon sizing decisions.
Prevents nil method error if campaign code paths are hit.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Integration Test — Verify AI Initializes and Builds

**Files:** None (testing only)

**Context:** With the instruction budget increased from 1M to 10M, the AI builder setup chain should complete successfully. This is the critical validation step — we need to verify that the AI threads survive initialization and begin issuing build orders.

- [ ] **Step 1: Run AI skirmish with 3000 ticks**

Run:
```bash
./build/Debug/opensupcom.exe --ai-skirmish --ticks 3000 --map "//maps/SCMP_009/SCMP_009_scenario.lua" 2>&1 | tee /tmp/ai_phase3.log
```

- [ ] **Step 2: Check for eliminated errors**

Run:
```bash
# Phase 2 errors should still be zero
grep -c "CanFormPlatoon" /tmp/ai_phase3.log
grep -c "GetPlatoonThreat" /tmp/ai_phase3.log

# Check remaining thread errors
grep "Thread error" /tmp/ai_phase3.log | sort -u

# Check for instruction budget kills (should be zero or near-zero)
grep -c "instruction count exceeded" /tmp/ai_phase3.log
```

Also verify personality wiring:
```bash
# Verify AIPersonality is being read and brain dispatch works
grep -i "AIPersonality\|OnCreateAI\|Initiating Archetype" /tmp/ai_phase3.log | head -5
```

Expected:
- `CanFormPlatoon` / `GetPlatoonThreat` errors: 0
- `instruction count exceeded`: 0 or near-zero (was the main blocker)
- `AIPersonality` / `OnCreateAI` log lines present (personality wiring works)
- Any remaining thread errors are new (not budget-related)

- [ ] **Step 3: Check for build activity**

Run:
```bash
# Look for factory construction
grep -i "start_build\|BuildStructure\|push_command" /tmp/ai_phase3.log | head -10

# Look for unit production
grep -i "FactoryFinish\|OnUnitStopBeingBuilt" /tmp/ai_phase3.log | head -10

# Look for platoon formation
grep -i "FormPlatoon:" /tmp/ai_phase3.log | head -10

# Check unit count progression
grep "Tick.*units alive" /tmp/ai_phase3.log | head -10
```

Expected: Evidence of AI building activity. Unit count should increase over time. If still 2 units at tick 3000, triage new errors in Step 4.

- [ ] **Step 4: Triage new thread errors**

Any new thread errors that appear fall into categories:

| Error Pattern | Likely Cause | Action |
|--------------|-------------|--------|
| `attempt to call method 'X' (a nil value)` | Missing moho method | Add stub or implementation |
| `instruction count exceeded` | Budget still too low | Increase budget further |
| `attempt to index.*a nil value` | Missing Lua global or table | Check import/load order |
| `attempt to perform arithmetic on.*nil` | Missing data in ScenarioInfo | Check lobby data flow |

Document findings. If new missing methods block the AI builder chain, add them as follow-up tasks before proceeding.

- [ ] **Step 5: If budget was insufficient, increase further**

If `instruction count exceeded` errors persist, increase budget in `thread_manager.hpp`:

```cpp
static constexpr i32 DEFAULT_INSTRUCTION_BUDGET = 50'000'000;  // 50M
```

Rebuild, retest. Repeat until AI threads complete initialization.

---

### Task 4: Fix Any Remaining Blockers

**Files:** Depends on findings from Task 3

**Context:** This task handles whatever new errors surface after the instruction budget increase. Common patterns from the spec:

1. **Missing `Buffs` global table** — `SetupCheat()` at `aiutilities.lua:2251` reads `Buffs['CheatBuildRate']`. If `BuffDefinitions.lua` hasn't been imported, `Buffs` is nil. Fix: verify `import('/lua/sim/BuffDefinitions.lua')` is called during AI init. If not, it's typically imported by `Unit.lua` during `OnCreate` — check if the import chain reaches it.

2. **Missing `EntityCategoryContains` global** — `ApplyCheatBuffs()` at `aiutilities.lua:2271` calls `EntityCategoryContains(categories.COMMAND, unit)`. This should be registered in sim globals. If missing, add a stub.

3. **Missing `AddInitialEnemyThreat`** — `adaptive-ai.lua:105` calls `self:AddInitialEnemyThreat(200, 0.005)`. This is a Lua method defined in `base-ai.lua`. If missing, verify `base-ai.lua` import chain.

4. **ScenarioInfo.Options.CheatMult/BuildMult missing** — `SetupCheat()` reads these. They come from lobby options (`AIOpts` in `lobbyOptions.lua`). If missing, the `tonumber()` calls return nil, and buff multipliers won't apply. For `--ai-skirmish` mode, set reasonable defaults.

For each blocker found:
- [ ] Add the fix (stub or implementation)
- [ ] Build and test
- [ ] Commit with descriptive message

---

### Task 5: Validate Cheat Buff Pipeline (If Applicable)

**Files:** None or `src/lua/moho_bindings.cpp` (if stubs needed)

**Context:** The cheat system pipeline is: `OnCreateAI` detects "cheat" suffix → `AIUtils.SetupCheat(brain, true)` → modifies `Buffs['CheatBuildRate']` and `Buffs['CheatIncome']` multipliers → calls `ApplyCheatBuffs(unit)` for each pool unit → `Buff.ApplyBuff(unit, 'CheatIncome')` / `Buff.ApplyBuff(unit, 'CheatBuildRate')` → `BuffAffectUnit` → `BuffEffects.BuildRate` calls `unit:SetBuildRate(val)` / `BuffEffects.EnergyProduction`/`MassProduction` sets adjustment modifiers.

All terminal moho methods are implemented:
- `SetBuildRate` (moho_bindings.cpp:2314)
- `SetProductionPerSecondMass` (moho_bindings.cpp:2242)
- `SetProductionPerSecondEnergy` (moho_bindings.cpp:2236)

This task validates the full chain runs without errors. It can be deferred if cheat variants aren't selected in `--ai-skirmish` mode.

- [ ] **Step 1: Run AI skirmish with cheat personality**

If the engine supports specifying AI personality via command line:
```bash
./build/Debug/opensupcom.exe --ai-skirmish --ticks 1000 --map "//maps/SCMP_009/SCMP_009_scenario.lua" 2>&1 | grep -i "cheat\|buff\|SetBuildRate" | head -20
```

If not, verify by checking if `ScenarioInfo.ArmySetup` contains "cheat" in the personality string during a normal `--ai-skirmish` run. If it doesn't (default is "adaptive"), this task is deferred until the lobby properly passes cheat personalities.

- [ ] **Step 2: Document any buff pipeline errors**

If errors appear in the buff chain, document and fix. Common issues:
- `Buffs` global is nil → need to import BuffDefinitions.lua earlier
- `Buff.ApplyBuff` hits nil → need to check import of Buff.lua
- `unit:SetBuildRate` error → verify method signature

---

### Task 6: Final Integration Validation

**Files:** None (testing only)

- [ ] **Step 1: Run extended AI skirmish**

Run:
```bash
./build/Debug/opensupcom.exe --ai-skirmish --ticks 5000 --map "//maps/SCMP_009/SCMP_009_scenario.lua" 2>&1 | tee /tmp/ai_phase3_final.log
```

- [ ] **Step 2: Verify success criteria**

```bash
# Unit count should increase (AI building)
grep "Tick.*units alive" /tmp/ai_phase3_final.log

# Thread errors should be minimal
grep -c "Thread error" /tmp/ai_phase3_final.log

# No instruction budget kills
grep -c "instruction count exceeded" /tmp/ai_phase3_final.log

# Summary
tail -20 /tmp/ai_phase3_final.log
```

Expected (from spec):
- AI builds at least 1 factory within 1000 ticks
- AI produces at least 1 unit within 2000 ticks
- AI sends a platoon to attack within 3000 ticks
- Thread error count for CanFormPlatoon/GetPlatoonThreat = 0
- No instruction budget kills

- [ ] **Step 3: Final build + full test suite**

```bash
cmake --build build --config Debug 2>&1 | tail -5 && ./build/tests/Debug/osc_tests.exe 2>&1 | tail -3
```
Expected: Build clean, all tests pass.
