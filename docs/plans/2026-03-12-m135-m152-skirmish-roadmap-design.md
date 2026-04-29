# OpenSupCom Engine: Single-Player Skirmish Roadmap (M135–M152)

**Date:** 2026-03-12
**Goal:** Run a complete single-player skirmish game of Supreme Commander: Forged Alliance with the original FA Lua UI driving all game screens.
**Approach:** Vertical slices — each milestone delivers a visible, testable result. FA's actual Lua code drives all UI panels; existing C++ HUD renderers (minimap, economy bars, strategic icons, overlays) are kept alongside.

---

## Decisions

- **Scope:** Skirmish only (no campaign, no multiplayer networking). Campaign framework is a follow-up project.
- **UI strategy:** Drive FA's real Lua UI code (`construction.lua`, `orders.lua`, `gamemain.lua`, `main.lua`, `lobby.lua`, etc.). The engine implements missing moho bindings so these Lua files run unmodified.
- **HUD strategy:** Hybrid — keep existing C++ HUD renderers, add Lua-driven panels on top. Individual C++ elements can be migrated to Lua later if visual fidelity requires it.
- **Milestone numbering:** Continues from M134 (bloom post-processing, the most recent completed milestone).
- **Bootstrap strategy:** M135 implements a minimal engine state machine (INIT→GAME with `StartGameUI()`) so in-game UI can be tested immediately. M144 later formalizes this into the full INIT→FRONT_END→LOADING→GAME→SCORE cycle needed for menu integration.

### Existing Binding Status

Many bindings listed in this roadmap already exist as stubs or partial implementations in `moho_bindings.cpp`, `sim_bindings.cpp`, and `engine_bindings.cpp`. Each milestone should audit existing code before implementing from scratch. Known existing stubs/implementations include:
- **WorldView:** `moho.UIWorldView` with `Project`, `ZoomScale`, `SetCartographic`, `IsCartographic`, `LockInput`, `UnlockInput`, `SetHighlightEnabled`, `GetsGlobalCameraCommands`, `SetBuildGhost`, `ClearBuildGhost`
- **WldUIProvider:** `InternalCreateWldUIProvider` exists in moho_bindings.cpp
- **Selection/Units:** `GetSelectedUnits`, `SelectUnits`, `unit:GetBlueprint`, `unit:GetEntityId`, `unit:GetArmy`, `unit:GetWorkProgress`, `unit:GetBuildRate`, `unit:GetCommandQueue`, `unit:GetStat`, `unit:IsIdleState`, `unit:GetFocusUnit`, `unit:GetGuardedUnit`
- **Categories:** `EntityCategoryFilterDown`, `EntityCategoryContains` in `sim_bindings.cpp`
- **State:** `GetFireState`, `SetFireState`, `SetPaused`, `GetIsPaused` on unit_methods
- **File I/O:** `DiskFindFiles`, `DiskGetFileInfo`, `exists` in `engine_bindings.cpp`
- **UI:** `SetCursor`, `GetFrame`, `GetNumRootFrames`
- **Session:** `SessionIsReplay`, `SessionGetScenarioInfo`, `IsAlly`, `IsEnemy`

Each milestone's work is upgrading stubs to real implementations and adding genuinely missing bindings.

### Architectural Constraints

These constraints apply across all milestones and must not be violated:

**1. Sim/UI Lua State Isolation (affects M135, M138)**
The engine MUST maintain two separate Lua VMs: one for Sim, one for UI. `SimCallback` (M138) is the only bridge — it serializes the `Args` table across the boundary, never passing raw Lua table pointers. If Sim and UI share a single Lua state, the UI (which runs at variable frame rate and responds to user input) can corrupt Sim state, breaking the deterministic lockstep required for replays and future multiplayer. The current engine uses a single `lua_State*` — M135 must split this into `sim_L` and `ui_L`, with `SimCallback` performing deep-copy serialization between them.

**2. Input Event Swallowing & Z-Order (affects M136, M150)**
Because the HUD is hybrid (C++ renderers + Lua panels layered on top), the input dispatch system must correctly "swallow" mouse clicks. When a user clicks a Lua UI button (e.g., "Build Factory" in the construction panel), that event MUST be consumed and stop propagating. If it falls through to the WorldView layer underneath, the game will simultaneously register a UI click and a terrain click, issuing an accidental move command. The existing `UIDispatch::hit_test()` already walks front-to-back and the `fire_handle_event` returns a consumed flag — this pattern must be extended to properly gate WorldView click-through when Lua panels are active.

**3. Heightmap Raycasting Performance (affects M136, M139)**
`GetMouseWorldPos()` requires projecting a screen ray onto the terrain heightmap. Brute-force linear stepping along the ray is too expensive for FA's highly tessellated terrain, especially when the camera is zoomed out and the ray is near-parallel to the ground. FA's UI calls this function constantly during build mode (M139). Implementation should use a hierarchical approach — either a terrain quadtree with min/max height bounds per cell, or a hierarchical Z-buffer — to achieve O(log N) intersection time rather than O(N).

**4. Async UI Icon Loading (affects M140)**
When the player first selects an engineer, `construction.lua` will request dozens of blueprint icon textures simultaneously. If `TextureCache` synchronously extracts `.dds` files from `.scd` archives and uploads to the GPU on the main thread, the game will hitch visibly. The existing `TextureCache` already supports async loading via `std::async` + `flush_uploads()` per frame — M140 must use this path for UI icons, returning a placeholder descriptor (e.g., transparent 1x1 pixel) immediately and swapping in the real texture when the async load completes.

**5. LOC() Lookup Performance (affects M135)**
FA wraps virtually every UI string with `LOC(key)`. The engine must load `/loc/us/strings_db.lua` at startup and cache all key→string mappings in a C++ `std::unordered_map<std::string, std::string>`. `LOC()` then does a single hash lookup and pushes the result. If the implementation executes a Lua pcall or VFS read per invocation, it will become a measurable bottleneck in per-frame beat functions that rebuild UI text every tick.

---

## Phase 1: WorldView & Command Infrastructure (M135–M139)

Foundation that all in-game Lua UI panels depend on.

### M135: WldUIProvider & Game Entry Points

The engine must expose a `moho.WldUIProvider_methods` C++ class so that FA's `gamemain.lua` can construct the entire in-game HUD. Also establishes foundational utilities needed by all subsequent milestones.

**Deliverables:**
- **Sim/UI Lua state split:** separate `sim_L` and `ui_L` VMs (see Architectural Constraint #1)
- `WldUIProvider` C++ class following the `_c_object` lightuserdata pattern (upgrade existing `InternalCreateWldUIProvider` stub)
- Minimal engine state machine: INIT→GAME with `StartGameUI()` call (formalized in M144)
- Engine state transitions call into Lua:
  - `SetupUI()` — called before every UI state change (loads cursor, prefs, skin)
  - `StartGameUI()` — triggers `CreateWldUIProvider()` from `gamemain.lua`
- `CreateGameInterface(isReplay)` callback — gamemain.lua builds the entire HUD here
- Engine-to-Lua callbacks: `OnFirstUpdate()`, `OnBeat()`, `OnSelectionChanged(oldSel, newSel, added, removed)`
- Engine globals: `GetCurrentUIState()`, `WorldIsLoading()`, `SessionGetScenarioInfo()`, `GetArmiesTable()`, `GetEconomyTotals()`
- Foundational utilities (needed by all FA Lua code from first boot):
  - `LOC(key)` / `LOCF(key, ...)` — localization (loads from `/loc/us/strings_db.lua` via VFS)
  - `GetPreference(key, default)` / `SetPreference(key, val)` — backed by JSON file on disk
  - `WaitSeconds(n)` / `WaitTicks(n)` — UI coroutine suspension (complement sim-side `WaitFor`)
  - `ForkThread(func, ...)` — UI-side coroutine spawning
  - `FlushEvents()` — drain input event queue during transitions

**Files touched:** `src/lua/moho_bindings.cpp`, `src/main.cpp`, new `src/ui/wld_ui_provider.hpp/cpp`, new `src/core/preferences.hpp/cpp`

### M136: WorldView Control

The WorldView is a moho UI control that represents the 3D game world within the UI tree. FA's `worldview.lua` expects it as a proper control with camera, projection, and input.

**Deliverables:**
- WorldView as a UIControl subclass with `_c_object` pattern (upgrade existing `moho.UIWorldView` stubs to real implementations)
- Camera integration (upgrade existing stubs):
  - `worldView:Project(worldPos)` — 3D world position → 2D screen coordinates
  - `worldView:ZoomScale(x, y, wheelRotation, wheelDelta)` — mouse wheel zoom
  - `worldView:SetCartographic(bool)` / `IsCartographic()` — top-down mode toggle
- `GetMouseWorldPos()` — screen ray → terrain intersection using hierarchical quadtree (see Architectural Constraint #3)
- `worldView:HitTest(x, y)` — determine what's under the cursor (unit, terrain, resource)
- Input routing: click on WorldView determines target, feeds into command mode
- Methods (upgrade existing stubs): `LockInput()`, `UnlockInput()`, `IsInputLocked()`, `SetHighlightEnabled()`, `GetsGlobalCameraCommands()`
- `worldView:Register(cameraName)` — associates a named camera; `GetCamera(cameraName)` returns it
- Camera moho class (`camera_methods`):
  - `GetCamera(name)` — retrieve named camera
  - `camera:SaveSettings()` / `camera:RestoreSettings(settings)` — camera state save/restore
  - `camera:SetZoom(zoom, duration)` / `camera:GetZoom()` — zoom control
  - `camera:RevertRotation()` — reset rotation
  - `UIZoomTo(units, duration)` — animate camera to unit positions (also used by M150 control groups)

**Files touched:** new `src/ui/world_view.hpp/cpp`, `src/lua/moho_bindings.cpp`

### M137: Selection↔Lua Bridge

FA's UI code constantly queries and modifies the unit selection. The engine must expose selection as proper Lua unit userdata.

**Deliverables:**
- `GetSelectedUnits()` returns Lua array of unit objects with proper metatables (not just entity IDs)
- `SelectUnits(unitArray)`, `AddSelectUnits(unitArray)` from Lua side
- `OnSelectionChanged(oldSel, newSel, added, removed)` callback fires into gamemain.lua on every selection change
- `EntityCategoryFilterDown(category, units)` — filter unit array by category expression (upgrade existing sim_bindings stub)
- `EntityCategoryContains(category, unit)` — test single unit against category (upgrade existing stub)
- `ValidateUnitsList(units)` — filter out destroyed units from an array
- `unit:IsInCategory(category)` — test single unit against category name
- `unit:GetCreator()` — return unit's builder (for factory chains)
- `unit:GetAttachedUnitsList()` — return transport cargo / attached units
- `unit:GetEconData()` — return `{energyConsumed, massProduced, ...}` for economy display

**Files touched:** `src/lua/moho_bindings.cpp`, `src/sim/sim_state.hpp/cpp`

### M138: Command Mode & SimCallback

The bridge between UI clicks and sim-side actions. FA's `commandmode.lua` and `SimCallbacks.lua` define how the player issues orders.

**Deliverables:**
- `SimCallback({Func=name, Args=data})` — Lua UI → sim bridge. Calls registered handlers in SimCallbacks.lua
- `GetUnitCommandData(units)` → returns `(availableOrders, availableToggles, buildableCategories)`:
  - `availableOrders`: list of command caps the selected units support
  - `availableToggles`: toggle states (fire state, shield, cloak, etc.)
  - `buildableCategories`: entity category expression of what can be built
- `IssueBlueprintCommand(orderType, bpId, count, clearQueue)` — queue a build order
- `IssueBlueprintCommandToUnit(unit, cmdType, bpId, count, clearOrders)` — build order to specific unit
- State queries: `GetFireState(units)`, `GetIsAutoMode(units)`, `GetIsPaused(units)`, `GetIsSubmerged(units)`
- State setters: `SetPaused(units, bool)`, `SetFireState(units, stateId)`, `SetAutoMode(units, bool)`

**Files touched:** `src/lua/moho_bindings.cpp`, new `src/lua/sim_callbacks.hpp/cpp`

### M139: Build Placement System

When the player clicks a build button, they enter build mode — ghost preview follows cursor, click places the structure.

**Deliverables:**
- `commandmode.lua` integration — enter/exit build mode, cursor changes to build cursor
- `GetMouseWorldPos()` snap-to-grid for structure placement (grid step matches footprint)
- Build restriction checks: footprint collision with existing structures, terrain slope limits
- `AddCommandFeedbackBlip(meshInfo, duration)` — visual click confirmation effect
- `ClearBuildTemplates()` — cleanup on mode exit
- Build ghost rendering reuses existing `inject_ghost()` from UnitRenderer, driven by Lua build mode state

**Files touched:** `src/lua/moho_bindings.cpp`, `src/sim/sim_state.hpp/cpp`, `src/renderer/unit_renderer.cpp`

---

## Phase 2: In-Game Lua UI Panels (M140–M143)

With WorldView and commands wired, FA's HUD Lua files can boot and function.

### M140: Construction Panel

The build menu showing what selected units can build, organized by tech level.

**Deliverables:**
- Wire `construction.lua` — build buttons appear based on `buildableCategories` from `GetUnitCommandData()`
- `EntityCategoryGetUnitList(category)` — returns array of blueprint IDs matching a category expression
- Blueprint icon texture resolution from blueprint `Display.IconPath` (loaded via TextureCache)
- Factory queue display:
  - `SetCurrentFactoryForQueueDisplay(factory)` — returns current build queue array
  - `PeekCurrentFactoryForQueueDisplay(factory)` — query without setting current
  - `ClearCurrentFactoryForQueueDisplay()` — cleanup
  - `DecreaseBuildCountInQueue(index, count)` — remove items from queue
- Build queue entries: `{type, id (blueprintId), count}`

**Files touched:** `src/lua/moho_bindings.cpp`

### M141: Orders Panel

The 14-slot command button grid (stop, move, attack, guard, patrol, repair, reclaim, capture, etc.).

**Deliverables:**
- Wire `orders.lua` — buttons populated from `availableOrders` / `availableToggles`
- `GetOrderBitmapNames(bitmapId)` — returns texture path for each order button icon
- `GetUnitCommandFromCommandCap(cap)` — converts command capability string to command enum
- Toggle button state display (fire state icon changes with current state, shield on/off, etc.)
- `IssueUnitCommand(units, command)`, `IssueCommand(command)` — generic command issuance

**Files touched:** `src/lua/moho_bindings.cpp`

### M142: Unit Info Panel

Displays selected or hovered unit stats — health, shield, weapons, economy impact, veterancy.

**Deliverables:**
- Wire `unitview.lua` — shows unit info on selection or hover
- `GetRolloverInfo()` — returns rich struct:
  ```
  {blueprintId, entityId, customName, health, maxHealth,
   shieldRatio, fuelRatio, massProduced, massConsumed,
   energyProduced, energyConsumed, kills, workProgress,
   tacticalSiloStorageCount, tacticalSiloMaxStorageCount,
   nukeSiloStorageCount, nukeSiloMaxStorageCount,
   userUnit (Unit object), armyIndex, focusUpgrade, focus}
  ```
- Unit methods: `unit:GetMissileInfo()`, `unit:GetStat(name, default)`, `unit:GetWorkProgress()`
- Enhancement display: `EnhancementCommon.GetEnhancements(entityId)` — list installed ACU upgrades with slot info

**Files touched:** `src/lua/moho_bindings.cpp`

### M143: Tooltips & Announcements

Hover information and in-game event messages.

**Deliverables:**
- Tooltip system for build icons, order buttons, unit hover
- `StartCursorText(x, y, text, color, time, flash)` — floating text near cursor
- `announcement.lua` integration — event messages ("Unit built", "Commander under attack", "Insufficient energy")
- Preference-driven tooltip delay/style via `GetPreference()`

**Files touched:** `src/lua/moho_bindings.cpp`, `src/ui/ui_renderer.cpp`

---

## Phase 3: Game Session Flow (M144–M146)

Engine state machine, per-frame callbacks, and end-game flow.

### M144: Engine State Machine

Formalize the minimal state machine from M135 into the full game flow needed for menus and session transitions.

**Deliverables:**
- State enum: `INIT → FRONT_END → LOADING → GAME → SCORE → FRONT_END`
- Each transition calls `SetupUI()` (cursor, prefs, skin initialization)
- `GetCurrentUIState()` returns `"front-end"` or `"game"` (upgrade M135's minimal version)
- `StartGameUI()` triggers `CreateWldUIProvider()` → `CreateGameInterface(isReplay)`
- `StartFrontEndUI()` triggers `main.lua:CreateUI()` — new entry point for menu state
- `LaunchSinglePlayerSession(sessionConfig)` — transition from FRONT_END → LOADING → GAME
- Lua state management: globals properly initialized for each state
- `FlushEvents()` during state transitions (already implemented in M135)

**Files touched:** `src/main.cpp`, new `src/core/game_state.hpp/cpp`

### M145: Beat System & Session Sync

Per-frame Lua callbacks and game session queries that HUD panels depend on.

**Deliverables:**
- `AddBeatFunction(func, bool)` / `RemoveBeatFunction(func)` — register per-frame Lua callbacks
- Time queries: `GetGameTime()` (formatted), `GetGameTimeSeconds()`, `GetSimRate()`, `GameTick()`
- `SessionGetScenarioInfo()` — returns map name, options, army config, playable area dimensions
- Speed control: `SetGameSpeed(speed)`, `GetGameSpeed()`, `ConExecute("WLD_GameSpeed N")`
- Pause: `SessionRequestPause()`, `SessionResume()`, engine calls `OnPause()`/`OnResume()` into Lua

**Files touched:** `src/lua/moho_bindings.cpp`, `src/main.cpp`

### M146: Score Screen & Game Over

End-game display and return-to-menu flow.

**Deliverables:**
- Engine calls `NoteGameOver()` when EndGame fires
- `NoteGameOver()` sets `SetFocusArmy(-1)` (observer mode), shows score screen
- `score.lua:CreateScoreUI()` — per-army stats display:
  ```
  currentScores[armyId] = {
    general = {score, currentunits, currentcap},
    resources = {massin={rate}, massout={rate}, energyin={rate}, energyout={rate},
                 storage={maxMass, storedMass, maxEnergy, storedEnergy}},
    Defeated = bool
  }
  ```
- "Return to menu" transitions back to `FRONT_END` state
- `EscapeHandler()` — context-sensitive dialog (quit/resume/surrender depending on state)

**Files touched:** `src/lua/moho_bindings.cpp`, `src/main.cpp`

---

## Phase 4: Main Menu & Game Setup (M147–M149)

The front door — what the player sees on launch.

### M147: Main Menu

The FA main menu with campaign/skirmish/options/exit buttons.

**Deliverables:**
- Engine calls `StartFrontEndUI()` on startup → `main.lua:CreateUI()` builds the menu
- `LaunchSinglePlayerSession(sessionConfig)` — engine receives config, transitions to LOADING → GAME
- `ExitApplication()` — clean shutdown
- `GetFrontEndData(key)` / `SetFrontEndData(key, val)` — cross-state communication
- Global audio bindings: `PlaySound(soundTable)` / `StopSound(handle)` / `PlayVoice()` / `PauseSound()` / `PauseVoice()` / `EnableWorldSounds()` — used by menu music and in-game UI sounds
- Movie stub: `Movie` class for background video (stub with static image; actual SFD playback is polish)

**Files touched:** `src/lua/moho_bindings.cpp`, `src/main.cpp`

### M148: Skirmish Lobby (Simplified)

Game setup screen — no networking, just single-player AI opponent configuration.

**Deliverables:**
- Wire `lobby.lua:CreateLobby()` / `HostGame()` without networking layer
- Map selection: `DiskFindFiles("/maps/", "*.scenario.lua")` to enumerate maps
- Map preview: scenario info + heightmap thumbnail rendering
- AI opponent setup: faction, AI type/difficulty from `aitypes.lua`
- Player slot configuration: army assignment, team, color, faction
- "Launch" button builds `sessionConfig` → `LaunchSinglePlayerSession()`
- Prefs persistence: `GetPreference()` / `SetPreference()` for last-used map/settings

**Files touched:** `src/lua/moho_bindings.cpp`, `src/main.cpp`

### M149: Options & Preferences

Settings dialog and persistent user preferences.

**Deliverables:**
- Wire `options.lua:CreateDialog()` — graphics, gameplay, audio settings
- Profile system: `Prefs.GetFromCurrentProfile(key)` / `SetToCurrentProfile(key, val)` (builds on `GetPreference`/`SetPreference` from M135)
- Skin selection: faction-themed UI skins (UEF, Aeon, Cybran, Seraphim) via `UIUtil.SetCurrentSkin()`
- Layout selection: bottom/right panel layouts
- Key binding display (read-only; rebinding is future polish)

**Files touched:** `src/lua/moho_bindings.cpp`

---

## Phase 5: Integration & Polish (M150–M152)

Final stretch — hotkeys, chat, and end-to-end verification.

### M150: Hotkeys & Control Groups

Keyboard shortcuts and unit group management.

**Deliverables:**
- `controlgroups.lua` — Ctrl+1-9 save selection, 1-9 recall, double-tap to zoom
- `IN_AddKeyMapTable(keymap)` / `IN_RemoveKeyMapTable(keymap)` — register/unregister hotkey sets
- Build hotkeys: T1/T2/T3 category shortcuts for construction panel
- Order hotkeys: A=attack, M=move, S=stop, P=patrol, G=guard, R=reclaim, E=repair, etc.
- `IsKeyDown(keyName)` — modifier queries for Shift (queue), Ctrl (append), Alt
- Camera bookmarks: Ctrl+F5-F8 save position, F5-F8 recall

**Files touched:** `src/lua/moho_bindings.cpp`, `src/ui/ui_dispatch.cpp`

### M151: Chat & Diplomacy (Minimal)

In-game messages and basic ally interaction.

**Deliverables:**
- `chat.lua` — message display (single-player: system messages, announcements)
- `RegisterChatFunc(func, name)` — handler registration for incoming messages
- `SessionSendChatMessage(clients, msgTable)` — stub for single-player
- System announcements: "Commander under attack", "Unit complete", "Insufficient energy"
- `SimCallback({Func='GiveResources', Args={...}})` — resource sharing with AI allies

**Files touched:** `src/lua/moho_bindings.cpp`

### M152: End-to-End Integration Test

Full-flow verification and gap filling.

**Deliverables:**
- Complete flow test: launch → main menu → skirmish setup → pick map + AI → load → play → build → fight → win/lose → score → return to menu
- Fix all missing binding stubs discovered during integration (expect 10-20 minor gaps)
- Loading screen: progress bar during map load (`StartLoadingDialog`, `UpdateLoadingDialog`, `StopLoadingDialog`)
- Cursor feedback: build placement valid/invalid indicators
- UI sound effects: button clicks, build complete, etc.
- Performance verification: Lua UI beat functions don't impact frame rate

**Files touched:** Various — gap-filling across all binding and UI files

---

## Summary

| Phase | Milestones | Deliverable |
|-------|-----------|-------------|
| 1: WorldView & Commands | M135–M139 | Lua can see the world, select units, issue commands, place buildings |
| 2: In-Game UI Panels | M140–M143 | Construction menu, orders panel, unit info, tooltips |
| 3: Game Session Flow | M144–M146 | State machine, beat system, score/game-over screen |
| 4: Main Menu & Setup | M147–M149 | Main menu, skirmish lobby, options/preferences |
| 5: Integration & Polish | M150–M152 | Hotkeys, chat, full end-to-end playable skirmish game |

**End state (M152):** A complete single-player skirmish game with FA's original Lua UI, running on the OpenSupCom Vulkan engine. Player launches the game, sees the FA main menu, sets up a skirmish against AI, plays the match with full construction/orders/unit-info panels, and sees the score screen on victory or defeat.

---

## Key Engine Bindings Index

Comprehensive list of engine globals and moho methods required, with milestone assignments:

### Session/State — M135, M144, M145, M147
`GetCurrentUIState` (M135/M144), `WorldIsLoading` (M135), `SessionGetScenarioInfo` (M135), `SessionIsReplay` (M135), `SessionRequestPause` (M145), `SessionResume` (M145), `LaunchSinglePlayerSession` (M144/M147), `ExitApplication` (M147)

### Selection — M137
`GetSelectedUnits`, `SelectUnits`, `AddSelectUnits`, `ValidateUnitsList`, `OnSelectionChanged` (callback)

### Units — M137, M142
`GetUnitById` (M137), `IsDestroyed` (M137), `GetRolloverInfo` (M142), `GetAttachedUnitsList` (M137), `unit:GetBlueprint` (existing), `unit:GetUnitId` (existing), `unit:GetEntityId` (existing), `unit:GetArmy` (existing), `unit:GetFocus` (existing), `unit:GetCommandQueue` (existing), `unit:GetWorkProgress` (existing), `unit:GetBuildRate` (existing), `unit:GetCreator` (M137), `unit:GetGuardedEntity` (existing), `unit:GetEconData` (M137), `unit:GetMissileInfo` (M142), `unit:GetStat` (existing), `unit:IsIdle` (existing), `unit:IsInCategory` (M137)

### Commands — M138, M139, M141
`SimCallback` (M138), `GetUnitCommandData` (M138), `IssueBlueprintCommand` (M138), `IssueBlueprintCommandToUnit` (M138), `IssueUnitCommand` (M141), `IssueCommand` (M141), `IssueClearCommands` (existing), `GetUnitCommandFromCommandCap` (M141), `GetOrderBitmapNames` (M141), `AddCommandFeedbackBlip` (M139), `ClearBuildTemplates` (M139)

### Categories — M137, M140
`EntityCategoryFilterDown` (M137, upgrade existing stub), `EntityCategoryContains` (M137, upgrade existing stub), `EntityCategoryGetUnitList` (M140)

### State Queries/Setters — M138
`GetFireState` (upgrade existing), `SetFireState` (upgrade existing), `GetIsAutoMode` (M138), `SetAutoMode` (M138), `GetIsPaused` (upgrade existing), `SetPaused` (upgrade existing), `GetIsSubmerged` (M138), `SetAutoSurfaceMode` (M138)

### Economy — M135
`GetEconomyTotals` (M135), `GetArmiesTable` (M135), `GetFocusArmy` (existing), `SetFocusArmy` (existing), `IsAlly` (existing), `IsEnemy` (existing), `IsObserver` (M146)

### Factory Queue — M140
`SetCurrentFactoryForQueueDisplay`, `PeekCurrentFactoryForQueueDisplay`, `ClearCurrentFactoryForQueueDisplay`, `DecreaseBuildCountInQueue`

### WorldView — M136
`worldView:Project`, `worldView:ZoomScale`, `worldView:SetCartographic`, `worldView:IsCartographic`, `worldView:HitTest`, `worldView:LockInput`, `worldView:UnlockInput`, `worldView:IsInputLocked`, `worldView:SetHighlightEnabled`, `worldView:GetsGlobalCameraCommands`, `worldView:Register`, `GetMouseWorldPos` (all upgrade existing stubs)

### Camera — M136, M150
`GetCamera` (M136), `camera:SaveSettings` (M136), `camera:RestoreSettings` (M136), `camera:SetZoom` (M136), `camera:GetZoom` (M136), `camera:RevertRotation` (M136), `UIZoomTo` (M150)

### Time — M145
`GetGameTime`, `GetGameTimeSeconds`, `GetSimRate`, `GameTick`, `CurrentTime`, `GetSystemTimeSeconds`, `SetGameSpeed`, `GetGameSpeed`

### Beat System — M145
`AddBeatFunction`, `RemoveBeatFunction`

### Preferences — M135
`GetPreference`, `SetPreference` (M135, foundational), `GetFrontEndData` / `SetFrontEndData` (M147)

### Input — M150
`IsKeyDown` (M150), `IN_AddKeyMapTable` (M150), `IN_RemoveKeyMapTable` (M150)

### UI — M135
`GetFrame` (existing), `SetCursor` (existing), `GetCursor` (existing), `GetNumRootFrames` (existing), `StartCursorText` (M143), `HideGameUI` (M145), `FlushEvents` (M135)

### Audio — M147
`PlaySound`, `StopSound`, `PlayVoice`, `PauseSound`, `PauseVoice`, `EnableWorldSounds`

### File I/O — existing
`DiskFindFiles` (existing), `DiskGetFileInfo` (existing), `exists` (existing)

### Console — existing
`ConExecute` (existing), `ConExecuteSave` (existing)

### Threading — M135
`ForkThread` (M135, UI-side), `WaitSeconds` (M135), `WaitTicks` (M135)

### Localization — M135
`LOC` (M135), `LOCF` (M135)

### Chat — M151
`RegisterChatFunc`, `SessionSendChatMessage`, `GetSessionClients`

### Enhancement — M142
`EnhancementCommon.GetEnhancements`, enhancement slot tracking (RCH, Back, LCH)

### WldUIProvider — M135
`CreateWldUIProvider` (Lua-side), `InternalCreateWldUIProvider` (C++ init, existing stub), provider methods: `StartLoadingDialog`, `UpdateLoadingDialog`, `StopLoadingDialog`, `CreateGameInterface`, `DestroyGameInterface`, `GetPrefetchTextures`
