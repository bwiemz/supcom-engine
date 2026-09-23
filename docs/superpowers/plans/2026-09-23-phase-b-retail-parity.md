# Phase B — Retail Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Unmodified retail FA 3599 goes front end → lobby → skirmish → score using its
own Lua, windowed, on Linux. The 56 `retail-gap` data tests move into `gate`.

**Architecture:** A binding-coverage report (M184) turns the long tail into a measured,
ratcheted list instead of whack-a-mole. The tail is then closed family by family (sim,
then UI), each with a data-backed test. Engine entry points follow Moho's semantics:
`doscript` plus hooks, and `import` for module-relative files.

**Tech Stack:** C++20, Lua 5.0 (LuaPlus dialect), CTest data labels, retail FA data via
Steam auto-discovery.

**Spec:** `docs/ROADMAP.md` §5 Phase B (M184–M189). The inputs are the Phase A baseline in
`tests/integration/data_tests.cmake` and memory note `retail-fa-boot`.

## Global Constraints

- **Data.** No FA data in the repo. The coverage *baseline* stores only API names (engine
  function and method names), never script content.
- **Every closed gap needs a check.** Each gap closed moves at least one `retail-gap` test
  to `gate`, or adds a unit test.
- **FAF must not regress.** Keep FAF paths working: no FAF-only assumption may be removed
  without a retail and FAF-agnostic replacement.
- **Faithful, not stubbed.** Stub only what is truly cosmetic, and tag it `// stub:
  cosmetic` so M184 classifies it.

---

### Task 1: Binding-coverage report (M184)

**Files:**
- Create: `src/lua/binding_coverage.{hpp,cpp}`, `tests/test_binding_coverage.cpp`,
  `tests/integration/binding_baseline_retail.txt`
- Modify: `src/main.cpp` (`--binding-coverage <out.json>` mode), `tests/integration/CMakeLists.txt`

**Design:**
1. **Registered set, gathered at runtime.** Boot the sim and UI Lua states the normal
   way, then collect:
   - global function and table names, per state;
   - `moho.<class>_methods` keys, per class.
   Runtime enumeration catches every registration style: `register_function`, method
   tables, and `rawset` shims.
2. **Referenced set, from a Lua-aware scan of every `*.lua` in the VFS.** The tokenizer
   skips strings, `--` and `#` comments, and long brackets. It collects:
   - `obj:Name(` method calls;
   - bare `Name(` global calls;
   - `moho.x_methods.Name` accesses.
   Names defined in Lua are excluded: `function Name`, `Name = function`, class method
   tables, and locals.
3. **Report** (JSON, and a text summary to stdout) with:
   - missing globals, per state;
   - missing methods;
   - first reference location for each;
   - counts.
   Methods are unattributed to a class, the same limitation as the Phase A prototype.
4. **Ratchet.** `data.binding_coverage` fails when a name *not* in
   `binding_baseline_retail.txt` is missing. Removing names from the baseline is expected;
   adding them requires a justification comment.

- [ ] Unit-test the tokenizer and scanner on inline Lua snippets: strings containing
  `:X(`, `#` comments, and definitions shadowing calls.
- [ ] Implement the runtime registered-set gathering behind `--binding-coverage`.
- [ ] Generate the initial baseline from retail data and commit the names file.
- [ ] Register `data.binding_coverage` (labels `data;gate`).
- [ ] Commit.

### Task 2: Test-harness robustness for retail (M185, part 1)

These retail-gap failures come from the tests themselves, not the engine:

- **Fixed entity ids.** Tests address the ACU as entity `#1`, but retail creates props and
  deposits first. Add a Lua test helper `__osc_test_first_unit(army)` and a C++ helper
  `test::first_unit_of_army(sim, army)`, and switch every "no entity 1" test over.
- **UI tests on the sim state.** Bitmap, Edit, Cursor and similar tests look up UI
  factories on `TestContext::L`, which is the sim state. Give `TestContext` a `ui_L` and
  move those tests onto it.
- **Unchecked `lua_rawget` crashes (7 `*-render` modes).** Guard every table access after
  a failed `do_string`.

- [ ] Each fix moves its modes from `retail-gap` to `gate` in `data_tests.cmake`.

### Task 3: Retail sim API tail (M185, part 2)

Drive the coverage report's sim-side missing list to zero for gameplay-relevant names.
Known first items:
- `aibrain:GetNoRushTicks`
- `platoon:GetFactionIndex`
- `platoon:PlatoonCategoryCount` and `PlatoonCategoryCountAroundPosition`
- `unit:GetHealth` on the retail unit class chain
- `SubmitXMLArmyStats` (a stats upload; a no-op is faithful offline)
- `IssueFormAttack`, `IssueFormPatrol`, `IssueOverCharge`, `IssueFactoryAssist`, `IssueEnhance`
- `CreateTrail`, `GetCueBank`, `SetIgnoreArmyCap`, `OkayToMessWithArmy`

**Exit:** `data.ai-test`, `data.combat-test` and `data.full-smoke-test` pass on retail. A
4-AI retail skirmish runs 10 game-minutes with 0 Lua thread errors.

### Task 4: Retail UI boot (M186)

- `uimain.lua` must be loaded with `import` semantics: retail uses module-relative
  `import('uiutil.lua')`. Mirror Moho's user-state init sequence: `userInit.lua` with its
  hooks, then `import('/lua/ui/uimain.lua').SetupUI()`.
- Front-end → lobby on retail: `data.lobby-flow-test` passes.

### Task 5: FA in-game UI (M187) and audio (M188)

These are split into their own plans once Tasks 1–4 land. They are large:
- `CreateGameInterface`
- WorldView as a UI control
- UI sound manager and bank lookup through the VFS

---

## Self-review

- **Coverage.** Every M184–M186 item maps to Tasks 1–4. M187–M189 are deliberately deferred
  to follow-up plans, because their scope depends on what the coverage report shows.
- **Placeholders.** Task 3's list is a known-first list. The full list is the M184 report's
  output, by design.
