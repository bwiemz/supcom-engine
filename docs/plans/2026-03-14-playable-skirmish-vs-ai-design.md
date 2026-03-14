# Playable Single-Player Skirmish vs AI — Design Spec

**Goal:** A human player can launch a skirmish from the lobby, select a map, choose an AI opponent with a difficulty level (Easy/Medium/Adaptive/Rush/Turtle/Tech + cheating variants), play a full game (build base, produce armies, fight), reach a win/lose screen, and return to lobby to play again.

**Approach:** Bottom-up, validation-first. Four phases, each producing a testable milestone. Each phase gets its own implementation plan.

**Prerequisites:** Milestones 1-163 complete (sim, renderer, UI framework, skirmish infrastructure, air/naval movement, veterancy, AI-vs-AI validation).

---

## Phase 1: Human Player Validation

**Goal:** Verify a human player can complete the core gameplay loop: select ACU → open construction menu → build structures → queue factory production → issue commands → fight.

**Approach:** Reuse the M153 smoke test pattern — run the engine as a human player with method interceptors active, systematically walk through each gameplay action, and triage every crash/stub/missing-method into fix-now vs. defer.

### Test Sequence

1. **Lobby → Game launch** — select map, set self as human + 1 AI slot, launch. Verify the 19-step reload sequence (M155) works end-to-end with a human army.

2. **Construction panel** — select ACU, verify the construction panel populates with buildable blueprints (T1 land factory, power generator, mass extractor). Click a blueprint, verify build ghost appears, click to place, verify the ACU walks over and builds it.

3. **Factory production** — select a built factory, verify the construction panel shows producible units. Click a unit blueprint, verify it queues. Verify the factory builds the unit and it spawns at the rally point.

4. **Orders panel** — select units, verify order buttons appear (Move, Attack, Patrol, Guard, Stop). Click an order, verify cursor mode changes, click on map, verify the command dispatches via SimCallback.

5. **Shift-queue commands** — Shift+right-click to queue multiple move waypoints. Verify SimCallback distinguishes `clear_commands=true` (standard click) from `clear_commands=false` (Shift-click). Verify the UI's ghosted command path lines update correctly as units complete orders and pop them from the queue. This is a critical UI/sim sync point — FA's UI maintains a local prediction of `GetCommandQueue()` for drawing paths before the sim executes them.

6. **Economy feedback** — verify mass/energy bars update during construction. Verify stalling feedback is visible when over-building.

7. **Game-over flow** — kill the AI's ACU (or let it kill yours), verify EndGame triggers, score screen appears, return-to-lobby works.

### Expected Fix Areas

- Missing ui_L moho bindings for construction panel population (EntityCategoryGetUnitList, GetUnitCommandData, etc.)
- SimCallback dispatch gaps for build/production commands from UI
- Order button wiring (FA's orders.lua calls UI moho methods)
- Any missing unit_methods or control_methods on the ui_L side

### Deliverable

Human can build a base, produce units, and fight on a land map. All crashes and stubs logged. Non-blocking issues documented for Phase 4.

---

## Phase 2: AI Builder Infrastructure

**Goal:** FA's adaptive AI autonomously builds a base, produces units, and sends them to attack — using the real FA Lua AI code, not custom shortcuts.

### Problem Statement

FA's AI runs through a chain of Lua managers: ExecutePlan (aiarchetype-managerloader.lua) → BuilderManager → EngineerManager / FactoryBuilderManager / PlatoonFormManager. These managers call moho methods on the brain and platoon objects. About 6-8 of those are currently stubs or missing, causing the AI threads to silently fail and never issue build orders. Many other methods called by the AI are either already implemented or are pure Lua (resolved through the class metatable chain, not moho methods).

### Methods to Implement

#### Moho methods to implement (C++ engine work required)

##### PlatoonFormManager (forms attack/defense platoons from ArmyPool)

| Method | Object | Current State | Implementation |
|--------|--------|---------------|----------------|
| `CanFormPlatoon(template, count, location, radius)` | platoon | **missing** | Iterate pool units, check categories vs template requirements, compare counts within radius of location. |
| `FormPlatoon(template, count, location, radius)` | platoon | **missing** | Extract matching units from ArmyPool into new platoon. Create platoon, assign units, return handle. |

##### FactoryBuilderManager (queues factory production)

| Method | Object | Current State | Implementation |
|--------|--------|---------------|----------------|
| `BuildPlatoon(template, factories, count)` | brain | **missing** | Order factories to build units from platoon template. Iterate template entries, call Unit::build_unit() per factory. |
| `CanBuildPlatoon(template, factories)` | brain | **missing** | Check factories have tech level and resources for template. Category + blueprint validation. |

##### PBM build location management

| Method | Object | Current State | Implementation |
|--------|--------|---------------|----------------|
| `PBMAddBuildLocation(location, radius, name, useCenter)` | brain | stub_noop | Store named build location (vector + radius + name) on ArmyBrain. Only used by campaign-ai.lua — may remain no-op for skirmish, but implement for completeness. |
| `PBMRemoveBuildLocation(name)` | brain | stub_noop | Remove named build location by name. Same scope as above. |

##### Supporting methods (verify, implement if missing)

| Method | Object | Notes |
|--------|--------|-------|
| `GetNumUnitsAroundPoint(category, pos, radius, alliance)` | brain | May exist. Verify alliance filtering works. |
| `CombinePlatoons(platoon1, platoon2)` | brain | Merge units from platoon2 into platoon1, disband platoon2. |
| `GetUnitBlueprint(blueprintId)` | brain | Push blueprint Lua table by ID via BlueprintStore. |

#### Already implemented (verify only, no C++ work)

| Method | Object | Status |
|--------|--------|--------|
| `SetResourceSharing(bool)` | brain | Already implemented at moho_bindings.cpp:6385 — calls `brain->set_resource_sharing()`. Verify it works. |
| `DecideWhatToBuild(builder, buildType, categories)` | brain | Already implemented at moho_bindings.cpp:5443 — iterates template, matches by building type, returns blueprint ID. Verify it works. |

#### Not needed — pure Lua (resolved through class metatable chain, NOT moho methods)

- `PBMSetEnabled(bool)` — defined in campaign-ai.lua / base-ai.lua as Lua method
- `ForceManagerSort()` — defined in aibrain.lua:1209 (deprecated empty function), overridden in adaptive-ai.lua
- `ForkAIThread(fn)` — defined in platoon.lua:130 as wrapper around moho `ForkThread`
- `StopAI()` — defined in platoon.lua:141 as `self.AIThread:Destroy()`
- `SetPlatoonData(table)` — defined in platoon.lua:75 as `self.PlatoonData = table.deepcopy(dataTable)`
- `HasBuilderList` — Lua field check (`self.BuilderList`), not a moho method
- BuilderManager.lua / EngineerManager.lua / FactoryBuilderManager.lua class logic — all pure Lua
- Builder priority sorting, condition evaluation, task assignment — all Lua

### Architectural Note: Threat Matrix

The AI heavily uses `brain:GetThreatAtPosition()`, `GetHighestThreatPosition()`, and `GetThreatsAroundPosition()` to decide where to attack and where to build defenses. These are already implemented as query-based functions (scanning all units per call), not backed by a persistent influence map. This works functionally but may become a performance bottleneck when the AI forms platoons and evaluates dozens of positions per beat. If profiling reveals threat queries as a hotspot, consider adding a periodically-updated spatial threat grid (e.g., every 10 ticks) that projects each unit's DPS/health onto cells. For Phase 2, the existing query-based implementations should be sufficient to unblock AI building.

### Validation

Run `--ai-skirmish` with 2 AI armies. Success criteria:
- AI builds at least 1 factory within 1000 ticks
- AI produces at least 1 unit within 2000 ticks
- AI sends a platoon to attack within 3000 ticks

---

## Phase 3: AI Personality & Difficulty

**Goal:** The lobby difficulty dropdown works. Selecting "Easy", "Adaptive", "Rush", "Turtle", "Tech", or any cheating variant produces a meaningfully different AI opponent.

### How FA's Difficulty System Works

FA has 6 base AI types and 6 cheating variants, defined in `aitypes.lua`:

| Key | Brain File | Behavior |
|-----|-----------|----------|
| easy/medium | medium-ai.lua | Simplified builders, basic strategy (note: `easy` key maps to medium-ai.lua per index.lua, not to the separate easy-ai.lua) |
| adaptive | adaptive-ai.lua | Dynamic, adapts to opponent |
| rush | rush-ai.lua | Aggressive early game |
| turtle | turtle-ai.lua | Heavy defense, fortification |
| tech | tech-ai.lua | Rushes to higher tech tiers |
| random | adaptive-ai.lua | Randomly picks strategy variant |
| *cheat variants | Same brain + CheatIncome/CheatBuildRate buffs | Resource/build rate multipliers |

Each AI type is a separate brain Lua file, not just parameter tuning. The personality system modulates within each type via 25 behavioral parameters (air emphasis, tank emphasis, attack frequency, defense emphasis, etc.).

### Selection Flow

Lobby sets `AIPersonality` string on `ScenarioInfo.ArmySetup[name]` → `simInit.lua` OnCreateArmyBrain looks up personality → maps to brain class via `aibrains/index.lua` → sets metatable → calls OnCreateAI → if "cheat" suffix, calls `AIUtils.SetupCheat()` which applies buff multipliers.

### What to Implement

#### 1. AIPersonality metatable

The `aipersonality_methods` metatable is currently `empty_methods`. FA's `aipersonality.lua` does `AIPersonality = Class(moho.aipersonality_methods)` and defines personality templates. The engine needs:

- Make `aipersonality_methods` non-empty with: `GetAirUnitsEmphasis`, `GetTankUnitsEmphasis`, `GetBotUnitsEmphasis`, `GetSeaUnitsEmphasis`, `GetPlatoonSize`, `AdjustDelay`
- Update `brain_GetPersonality` (currently returns hardcoded table with all emphasis at 0.5) to return a real personality object that reads from personality template tables

#### 2. Personality selection wiring

- Verify `SetArmyAIPersonality(name, personality)` (implemented M163) stores the personality string where `simInit.lua` can read it at `ScenarioInfo.ArmySetup[name].AIPersonality`
- Verify lobby UI passes the correct personality key when launching

#### 3. Brain class dispatch

- `simInit.lua` uses `aibrains/index.lua` to map personality key → brain class file. Pure Lua. Verify `import()` works for all 6 brain files.
- Each brain file inherits from `base-ai.lua` → `moho.aibrain_methods`. Should work if Phase 2's methods are in place.

#### 4. Cheating system

- Cheat detection: if personality contains "cheat", `AIUtils.SetupCheat(brain, true)` applies buffs
- `CheatIncome` buff: multiplies mass/energy production by `ScenarioInfo.Options.CheatMult`
- `CheatBuildRate` buff: multiplies build rate by `ScenarioInfo.Options.BuildMult`
- **Buff system pipeline validation (CRITICAL):** The cheat system depends on FA's buff pipeline: `SetupCheat()` → `Buff.ApplyBuff()` → `BuffAffectUnit()` → `BuffEffects` → moho calls (`SetBuildRate`, `SetProductionPerSecondMass/Energy`, `SetConsumptionPerSecondMass/Energy`). This pipeline must be validated end-to-end:
  - `CheatBuffs.lua` must be loaded and the `Buffs` global table populated
  - `BuffDefinitions.lua` must be importable
  - `Buff.ApplyBuff()` must traverse to the terminal moho calls without hitting missing methods
  - The moho methods at the end of the chain (`SetBuildRate`, `SetProductionPerSecondMass`, etc.) already exist — verify they're wired correctly
- **Buff base value preservation:** When `SetBuildRate(20)` overwrites a base rate of 10 via a cheat buff, the engine must preserve the original base value so it can be restored if the buff is removed. Verify that the C++ Unit class stores `base_build_rate_`, `base_mass_prod_`, etc. alongside current values, or that FA's Lua buff system handles restoration through its own stacking logic.

#### 5. Lobby options for difficulty

- Lobby must populate `ScenarioInfo.Options.CheatMult` and `ScenarioInfo.Options.BuildMult` from AI difficulty selection
- `aitypes.lua` defines `ratingCheatMultiplier` and `ratingBuildMultiplier` per AI type — these need to flow into ScenarioInfo.Options

#### Not needed (pure Lua)

- Builder priority differences between rush/turtle/tech — handled by different builder tables in each brain file
- Strategy adaptation in adaptive-ai — pure Lua thread logic
- Custom AI scanning (`CustomAIs_v2/`) — nice-to-have for later

### Validation

Launch skirmish with Rush AI vs Turtle AI. Rush AI should build factories and attack early. Turtle AI should build defenses and fortify. Both should produce meaningfully different games.

---

## Phase 4: Integration & Polish

**Goal:** Full games run reliably start-to-finish. Human player vs AI opponent produces a complete, satisfying game experience with no crashes or showstopper bugs.

### Testing Matrix

| Test | What it validates |
|------|-------------------|
| Human vs Easy AI, land map (The Pass) | Basic gameplay loop, AI builds slowly |
| Human vs Adaptive AI, Seton's Clutch | Multi-layer (land/air/naval), AI adapts |
| Human vs Rush AI, small map | AI aggression timing, early pressure |
| Human vs Turtle AI, any map | AI defensive posture, late game |
| Human vs Cheating Adaptive | Cheat buffs apply, AI has resource advantage |
| AI vs AI, 3 different maps | Stability over long games, no memory leaks |
| Lobby → game → score → lobby → game | Reload cycle, no stale state |

### Fix Areas

1. **Edge case crashes** — stale entity handles after mass death, thread errors from unexercised AI paths, out-of-bounds positions at map edges.

2. **Non-critical stubs** — triage each: implement if gameplay-affecting, leave as stub if cosmetic:
   - `PlayCommanderWarpInEffect` — visual only, skip
   - `PlayFxRollOffEnd` — visual only, skip
   - `SetupBuildBones` — visual only, skip
   - `FlattenMapRect` — terrain deformation, skip for now
   - `DrawCircle/DrawLine/DrawLinePop` — debug rendering, skip

3. **Performance** — verify ≥30 FPS with 100+ AI units on screen:
   - **Pathfinding avalanches:** When AI forms a 40-unit platoon and orders an attack-move, 40 A* requests hit Navigator in a single tick. On large maps (81km Seton's Clutch) this will freeze the engine. Mitigation: implement a pathfinding request queue/time-slicer — if a tick exceeds N path requests, defer the rest to the next tick (units wait in idle state for a few frames). Bonus optimization: compute A* only for the platoon leader, have other units use lightweight steering/flocking to follow.
   - **Lua 5.0 GC stutters:** Lua 5.0 uses a stop-the-world garbage collector. The AI's BuilderManager and PlatoonFormManager generate massive ephemeral table garbage every beat (evaluating hundreds of build conditions). Without intervention, this causes 50-200ms stutter spikes as the GC pauses the VM. Mitigation: manually call `lua_gc()` with a fixed time budget per frame in ThreadManager or SimState to amortize GC cost across ticks.
   - Threat queries with many units (spatial hash mitigates, but see Phase 2 architectural note)
   - Lua thread instruction budget tuning (currently 1M per resume)

4. **Game-over reliability** — all win/lose paths:
   - Human kills AI ACU → victory
   - AI kills human ACU → defeat
   - Human quits to lobby mid-game
   - Both ACUs die same tick → draw (note: existing EndGame logic may not handle simultaneous deaths — treat as potential implementation item, not just test case)

5. **Score screen** — verify stats populate from ArmyStat tracking (units built, killed, mass collected).

### Deliverable

A human can reliably play 5 consecutive skirmish games against different AI types without crashes, score screen shows meaningful stats, return-to-lobby works every time.

---

## Summary

| Phase | Key Deliverable | Est. New Methods | Dependencies |
|-------|----------------|-----------------|--------------|
| 1 | Human player can build and fight | ~15-25 fixes (TBD) | None |
| 2 | AI autonomously builds and attacks | ~6-8 moho methods + verification | Phase 1 |
| 3 | Difficulty levels and AI variants work | ~6-8 implementations + wiring | Phase 2 |
| 4 | Full games run reliably | Testing + edge case fixes | Phase 3 |

**Final success criteria:** Human launches skirmish → selects Adaptive AI (Hard) → plays a full land/air/naval game on Seton's Clutch → wins or loses → score screen → returns to lobby → launches another game with Rush AI (Cheating) → plays again. No crashes.
