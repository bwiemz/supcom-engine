# M191 — Sim/User split

Status: design, 2026-09-24. Phase C (architecture seams), brought forward after an external review of the codebase.

## Why

- **The libraries link in a cycle.** The Lua library links the renderer, the renderer links the blueprints, and the blueprints link Lua. That's the one cycle in the target graph (`cmake --graphviz`); the roadmap's older worries (core→lua, sim→blueprints) are gone.
- **What a cycle costs:** the targets don't mark real boundaries. Anything that uses the Lua library pulls in the renderer. A dedicated server, a headless test runner, a replay viewer, a threaded renderer and an alternate renderer all need Lua without the renderer.
- **The UI reads the live sim.** The renderer reads only per-tick snapshots (M190b). But the UI state's unit methods (`UserUnit:GetPosition`, `GetHealth`, ...) still resolve to live sim units, so the user side can still reach into the sim.
- **`moho_bindings.cpp` is 16,879 lines** and holds every class's bindings, sim and UI alike.

## What makes the cycle

`osc_lua` links `osc_renderer` for one file's sake: 34 of `moho_bindings.cpp`'s bindings use the renderer or its input handler:
- the camera class (`moho.camera_methods`: 16 methods for zoom, move, save and restore);
- `UIWorldView`'s `__init`, `Project`, `ProjectMultiple` and `Register`;
- `MapPreview:SetTextureFromMap`;
- 13 globals: `InternalCreateWldUIProvider`, `GetCamera`, `UIZoomTo`, `GetMouseWorldPos`, `IsKeyDown`, `GetRolloverInfo`, `GetSelectedUnits`, `SelectUnits`, `AddSelectUnits`, and the four that act on the selection: `SimCallback`, `IssueCommand`, `IssueBlueprintCommand` and `GetUnitCommandFromCommandCap`.

Besides the renderer and input-handler getters, they use 13 of the file's general helpers (`get_sim`, `check_entity`, `push_unit_for_ui`, `issue_player_order`, ...).

## The steps

1. **Break the cycle.**
   - **The move:** the 34 bindings go into a new library, `osc_lua_user` (`src/lua/user_bindings.cpp`), which links `osc_lua` and `osc_renderer`. `register_user_bindings` adds them after `register_ui_bindings`: the globals, the camera class, and the renderer-backed methods of `UIWorldView` and `MapPreview`. The app calls it; no unit test uses the moved bindings.
   - **The helpers:** the 13 general ones are declared in `src/lua/moho_bindings_internal.hpp`.
   - **The result:** `osc_lua` no longer links the renderer.
   - **The guard:** a test labelled `arch` (`tools/check_link_cycles.py`, over `cmake --graphviz`) fails if the targets form a cycle again, or if the sim, its Lua library or the blueprints come to depend on a layer above them.
   - An interface the renderer implements would also free the user bindings from the renderer. It isn't worth it until a second renderer exists.
2. **Split `moho_bindings.cpp` by class** into `src/lua/bindings/sim/` and `src/lua/bindings/ui/`. Only files change, not behaviour; the binding-coverage ratchet and the gate show nothing is lost. **Done:**
   - **Where things went:**
     - Each class's methods and method table moved to its own file: 11 sim files (entity, unit, navigator, projectile, weapon, aibrain, platoon, shield, manipulators, effects, blip) and 5 UI files (controls, text, lists, world, lobby).
     - A helper moved with its users when they all live in one file. Otherwise it stayed shared. `moho_bindings_internal.hpp` now declares 35 more helpers and the 38 method tables.
     - `moho_bindings.cpp` keeps the shared helpers, the UI state's globals and the registration: 4.2k lines, from 16k.
   - **How:** a script cut the file into top-level items, skipping strings, raw Lua strings and comments. It assigned each item by name and by its users, and wrote the items back unchanged. The only edits: a shared item loses `static`; a shared function's default arguments move to its declaration; the hand-laid method tables are fenced from the formatter, as `moho_classes` is.
   - **The check:** a token count of before and after differs by exactly those edits.
   - **The format ratchet:** it now counts a block moved between files as moved, not changed.
3. **The UI reads snapshots.** `UserUnit` methods read the tick's `WorldSnapshot` (M190b) rather than live units. The user side then reaches the sim only through commands and `SimCallback`, as in Moho.
   - **The audit** (2026-09-24). The UI state's units carry the sim's own `moho.unit_methods`, so a UI script's call runs against the live unit. Two sources said which methods the UI uses:
     - **Traced:** every method a unit was asked for in the game-UI, victory and 600-frame capture runs (a `/schook` hook wrapping `moho.unit_methods` in the UI state). That's 14 methods: `GetBlueprint`, `GetCommandQueue`, `GetEntityId`, `GetHealth`, `GetMaxHealth`, `GetPosition`, `GetWorkProgress`, `IsAutoMode`, `IsDead`, `IsIdle`, `IsInCategory`, `IsOverchargePaused`, `IsRepeatQueue`, `ProcessInfo`.
     - **Read:** every `unit:Method(` in retail's UI Lua that names a unit method. That adds `GetCreator`, `GetFuelRatio`, `GetMissileInfo`, `SetCustomName`, `AddSelectionSet` and `RemoveSelectionSet`. The rest are other classes' methods of the same name (controls' `Destroy`, world meshes' `SetMesh`).
   - **What changes the sim:**
     - `ProcessInfo` already went through the callback queue.
     - The selection sets are the UI state's own bookkeeping.
     - `SetCustomName` wrote the live unit. It is used by the rename dialog, and by `OnFirstUpdate`, which names the commander for its player. In multiplayer only that player's sim had the name, and a replay lost it. It now goes through the queue as a `CustomName` ProcessInfo pair, as Moho's does (`ProcessInfoPair(id, "CustomName", name)` in the decompiled `UserUnit`), and the sim names the unit at the tick. The sim's own scripts (the scenario names each commander for its army) still name it at once.
   - **A read that was wrong:** `IsInCategory` took only a category object. UserUnit's takes a name, and retail's UI passes names (`'COMMAND'`, `'FACTORY'`, a faction). So `OnFirstUpdate` never recognised the commander, and the construction panel's category checks all failed. Only the UI calls it, so the sim is unchanged.
   - **Next:** the 14 reads come from the snapshot rather than the live unit.

## Proof

- **Step 1:**
  - **The graph:** the target graph is acyclic, and the new test holds it.
  - **Tests:** every gate mode passes, UI ones included (`--gameui-test`, the golden captures, input and selection).
  - **Coverage:** the binding-coverage ratchet shows no binding lost, and the unit tests pass.
- **Step 2:** the same checks. The binding coverage and the whole gate are identical.
- **Step 3:** UI tests read units through snapshots, including one where the snapshot and the live sim differ mid-tick.

## Risks

- **Registration order.** The user bindings add methods to classes the base registration makes, so `register_user_bindings` must run after `register_ui_bindings`. It checks the classes exist and logs an error if not.
