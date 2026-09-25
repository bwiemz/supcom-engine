# A fresh UI Lua state for every game

Status: design, 2026-09-25. Phase C (architecture), as M191 step 4: the user layer's lifecycle.

## Why

Moho starts the front end and each game by rebinding its UI manager to a Lua state. `UI_StartFrontEnd()` and `UI_StartGameUI(state)` call `SetNewLuaState`, then `uimain.lua`'s entry point (faf-re `src/sdk/moho/ui/IUIManager.cpp`).

faf-re doesn't show the state being created. But two things show it is fresh each time:
- retail's scripts break on a reused one (below);
- `SetFrontEndData`/`GetFrontEndData` exist to carry data across the reset.

The engine keeps one UI state for the whole run, and retail's scripts assume they start fresh. Found while building M208a:
- **The unit view breaks on a second game.** `ui/game/unitviewDetail.lua` rebuilds its layout by setting its module global `View = nil`, and the layout then reads `import(...).View`. In a module imported by an earlier game, that read raises "access to nonexistent global variable View".
- **Any second game in a run is affected:** lobby → game → lobby → game; loading a save or a replay from the game menu; a restart.
- **State leaks from game to game:** module globals, forked UI threads, beat functions, key maps and chat all survive into the next game.

Loading from inside a game (M208) waits on this.

## What exists now

Mapped 2026-09-25 (feat/m208a-save-load):
- **The run's single state.** `App::ui_lua_state` is built once, in `App::boot_ui()`, and never closed until exit.
- **No transition recreates it.** The launch request, `execute_reload_sequence` and return-to-lobby all reuse it. They destroy the game interface (`DestroyGameInterface`), run `SetupUI` again, and call `CreateUI`/`StartGameUI` on the same state.
- **Objects that point into the state**, through its registry refs or pointers:
  - `ui_store` (blueprint refs) and `ui_thread_manager` (coroutines);
  - `ui_registry`'s controls (table refs);
  - `beat_registry` and `keymap_registry` (function and table refs);
  - `FrontEndData` (refs);
  - the `Engine` handle the test modes use;
  - UIDispatch's hover control (a `UIControl*`);
  - the sound manager's `WaitFor` callbacks, which capture the UI `ThreadManager`.
- **C++ objects published into the state's registry:**
  - App members: sound, preferences, localization, special files, the world UI provider, the thread manager, beat, keymap, `FrontEndData`, command-line args, the game-state manager;
  - `run_window`'s locals: renderer, input handler, factory queue, sim-callback queue;
  - the world source.
- **`FrontEndData` holds bare registry ids.** Its values live in the setting state's registry, and are often the lobby's own table, not a copy. They die with that state.

## Design

### A `UiState` object, one per front end or game

`App` stops owning the UI Lua state directly. It owns `std::unique_ptr<UiState> ui`, which holds everything that points into one state, and is destroyed with it:
- the `LuaState`;
- `ui_store`, `ui_thread_manager`, `ui_registry`;
- `beat_registry`, `keymap_registry`;
- the world UI provider's Lua side;
- the per-state registry keys.

Destroying a `UiState` goes in this order:
1. Destroy its control tree, running `OnDestroy`, while the state is still alive.
2. Cancel its sound waits.
3. Clear the dispatcher's hover and focus.
4. Drop its thread manager and registries.
5. Close the state.

`App` keeps what outlives a game: sound, preferences, localization, special files, the game-state manager, the renderer and input handler, the sim, and a state-independent `FrontEndData`.

### Building one

`App::boot_ui()` becomes `make_ui_state(Kind kind)`, with `kind` either FrontEnd or Game. It is today's boot, run on a new `LuaState`:
1. init;
2. UI blueprints;
3. bindings;
4. publishing the App's objects into the registry;
5. the session globals;
6. `userInit.lua`;
7. `SetupUI`.

The window's objects (renderer, input handler, factory queue, callback queue) are published by the same call; they move from `run_window`'s stack into `App`. The last step depends on the kind:
- **front end:** the main menu's `CreateUI` and the LAN dialog;
- **game:** nothing more. The launch runs `begin_world_ui`, then `finish_world_ui` after the sim reloads.

Loading blueprints into a new UI state takes about 5 s in Debug, paid once per transition, during the loading screen. The sim's reload already pays a load of its own. Copying the sim state's parsed blueprints across, instead of parsing them again, is a later optimisation.

### Transitions

- **Launch** (lobby, replay, saved game, and later the game menu's Load):
  1. Read the request from the current state.
  2. Build a new Game state and show its loading dialog.
  3. Destroy the old state.
  4. Reload the sim against the new state.
  5. Build the game interface.
- **Return to the front end:** tear the game down, destroy its state, and build a FrontEnd state, which shows the main menu.
- **The launch and return flags** (`__osc_launch_*`, `__osc_return_to_lobby`, `__osc_exit_requested`) move into a C++ `SessionRequests` on `App`. The bindings set it through a registry pointer. A flag set in a state that is about to close is then never lost, and the loop reads C++ fields instead of Lua keys.

### `FrontEndData` across states

`SetFrontEndData(key, value)` deep-copies the value into a private Lua state owned by `FrontEndData`, as `Preferences` already does for its values (`copy_lua_value`). `GetFrontEndData(key)` deep-copies it out into the asking state. Tables are copies: retail's scripts pass data forward (the session config, the replay file name, the next operation's briefing), never shared objects. `execute_reload_sequence` reads `sessionConfig` from it as before.

### What else must follow the state

- `Engine`, the test modes' handle, reaches the current `UiState` through `App`, not through references bound at construction.
- The smoke harness reinstalls on each new state.
- `first_update_fired` (a function static in the loop) becomes per-game state.
- The world source (`__osc_ui_world_source`) is published into each Game state.
- Headless test runs (`execute_reload_sequence` from test_modes.cpp) keep one state per run: they have no front end to return to.

## Proof

- **`data.relaunch_flow`**, offscreen: lobby → game → return to lobby → game, and a saved game loaded from inside a game, as the game menu's Load dialog does (M208's follow-up). Each game must build its interface with no Lua errors, including `unitviewDetail`'s layout. SessionIsReplay, the focus army and FrontEndData's session config must be right in each.
- **Unit:**
  - `FrontEndData` round-trips values between two Lua states, including nested tables and the copy semantics;
  - `SessionRequests` survives the state that set it.
- The existing gate, unchanged. It runs a single game, so its behaviour must not change.

## Also on the relaunch path (separate fixes)

Found alongside this in M208a. Each gets its own small change and test:
- **A GPU allocation leaks** on each relaunch: VMA asserts at exit in Debug. Find the per-scene resource that `clear_scene` doesn't free.
- **Per-map textures are cached by name** (`__terrain_blend0/1`, `__normal_overlay__`) and survive `clear_scene`. A second game on a different map draws the first map's blends. Evict them in `clear_scene`.

## Risks

- **Retail scripts that expect state from the front end in the game.** Moho resets too, so they don't, but our own glue might. The LAN dialog and the smoke harness are the known cases.
- **Load time:** one UI blueprint load per transition, about 5 s in Debug and less in Release.
- **This is a wide change in `window.cpp`.** Do it after M192 step 2c (the window loop's handlers) if that lands first, or as part of it.
