# Full Skirmish Completion — Design Spec

**Date:** 2026-04-05  
**Goal:** Every feature needed for a complete human-vs-AI skirmish game working end-to-end: lobby → loading → gameplay (all unit types T1-T3, land/air/naval, all abilities) → score → return to lobby.

---

## Phase 1: Lobby → Game → Score → Lobby Pipeline

### 1.1 Crash Fix (DONE)
- `dispatch_events` range-for loop over `pending_events_` invalidated by GLFW callbacks pushing new events during Lua pcall. Fix: swap into local copy before iterating.

### 1.2 Font Mapping (DONE)
- "Arial Gras" (French for "Arial Bold") → map to `/fonts/ARIALBD.TTF` in `FontCache::resolve_font_path`.

### 1.3 Control.Disable/Enable/IsDisabled
**Problem:** FA's Combo.HandleEvent calls `self:IsDisabled()`. Our base Control class has no disable state — only Edit controls have Enable/Disable.

**Fix:** Add to `control_methods` in moho_bindings.cpp:
- `Disable()` — sets `_isDisabled = true` on the Lua instance table (via lua_rawset)
- `Enable()` — sets `_isDisabled = false`
- `IsDisabled()` — reads `_isDisabled` from instance (lua_rawget), returns false if nil

These are pure Lua-table operations, no C++ state needed. FA's Combo/Button/BitmapCombo check `self:IsDisabled()` in HandleEvent and return early if true.

### 1.4 SendData Loopback for Single-Player
**Problem:** `lobby_SendData` is a no-op stub. FA's lobby sends `{Type='AddPlayer', PlayerOptions=...}` to the host via SendData. Without loopback, the host player is never added to slot 1 and AI players can never be added.

**Fix:** Make `lobby_SendData` call `lobbyComm.DataReceived(lobbyComm, data)` directly for single-player:
```
lobby_SendData(self, targetID, data):
  1. Read data table from arg 3
  2. Inject SenderID = localPlayerID, SenderName = localPlayerName into data
  3. Get lobbyComm from self (arg 1)
  4. Call lobbyComm:DataReceived(data) via lua_pcall
```

This simulates the network loopback that FA's engine provides. The DataReceived handler in lobby.lua processes AddPlayer, RequestColor, SetPlayerOption, etc.

### 1.5 SessionConfig → Game Wiring
**Problem:** `execute_reload_sequence` only reads `Human`, `AIPersonality`, `Faction` from sessionConfig. Missing: `StartSpot`, `Team`, `PlayerColor`, `ArmyColor`, `GameOptions.ScenarioFile`.

**Fix:** Extend the sessionConfig parsing block in main.cpp:
- **StartSpot:** Read `PlayerOptions[slot].StartSpot` (1-based index into scenario's army markers). Pass to SimState to assign spawn positions per army.
- **Team:** Read `PlayerOptions[slot].Team`. After all armies are created, call `SetAllianceToArmy(i, j, "Ally")` for armies on the same team (Team >= 2). Team 1 means "no team" (FFA).
- **PlayerColor/ArmyColor:** Read color indices. Store on ArmyBrain for renderer to read.
- **ScenarioFile:** Read `GameOptions.ScenarioFile` as the map path instead of the `__osc_launch_scenario` registry key (which is a fallback).

### 1.6 ItemList Compatibility
**Problem:** FA's Combo calls `self:AddItems(table)` and `self:ClearItems()`. We only have `AddItem(string)` and `DeleteAllItems()`.

**Fix:**
- Add `AddItems(table)` — iterates numeric keys 1..n, calls AddItem for each
- Add `ClearItems()` as alias for `DeleteAllItems()`
- Add `SetTitleText(text)` — stores title string on the Lua instance (Combo reads it for display)

---

## Phase 2: Missing Gameplay Mechanics

### 2.1 Cloaking System
**What FA does:** `EnableCloak()`/`DisableCloak()` toggle visibility. Cloaked units are invisible to enemies without Omni sensors. Cloaking drains energy per tick. If energy stalls, cloak drops.

**Implementation:**
- Add `cloaked_` bool on Unit
- `EnableCloak` sets `cloaked_ = true`, adds energy consumption from blueprint `Intel.CloakEnergyDrainPerSecond`
- `DisableCloak` clears it, removes consumption
- In visibility grid update: cloaked units are not revealed by Vision (only by Omni)
- In entity query filters: cloaked enemy units excluded from targeting unless seen by Omni
- Energy stall check: if army efficiency < threshold, force `DisableCloak`

### 2.2 Stealth System
**What FA does:** `EnableStealth()`/`DisableStealth()` make units invisible to Radar (but not Vision). `EnableSonarStealth()`/`DisableSonarStealth()` for Sonar.

**Implementation:**
- Add `radar_stealth_` and `sonar_stealth_` bools on Unit
- Stealth units don't appear on enemy radar overlay or minimap radar dots
- Already partially implemented via blip visibility filtering — need to wire the enable/disable API
- Energy drain from blueprint `Intel.RadarStealthFieldEnergyDrainPerSecond`

### 2.3 SetAutoMode
**What FA does:** Toggles automatic fire for TMD/anti-nuke, automatic assist for engineering stations.

**Implementation:**
- Add `auto_mode_` bool on Unit
- `SetAutoMode(bool)` / `GetAutoMode()` Lua bindings
- For TMD/anti-nuke: when auto_mode_ is true, weapon auto-targets incoming missiles (already handled by weapon targeting system — just need the toggle state readable by Lua)
- For engineering stations: when auto_mode_ is true, automatically assist nearby construction (handled in Lua via OnCreate thread)

### 2.4 OnAdjacentTo Callback
**Problem:** `OnNotAdjacentTo` fires when adjacent structures are removed, but `OnAdjacentTo` never fires when structures are placed.

**Fix:** In the unit placement/construction completion code (entity_OnStopBeingBuilt or finish_build), scan for adjacent structures using the spatial hash. For each adjacent structure pair, fire `OnAdjacentTo(self, adjacentUnit)` on both units. The adjacency scan uses the unit's footprint to find neighbors within 1 cell.

### 2.5 Adjacency Bonus Math
**How FA does it:** The bonus calculation is entirely in Lua. `OnAdjacentTo` callback in `defaultunits.lua` calls `ApplyAdjacencyBuffs()` which reads the adjacency bonus tables from blueprints and applies Buff multipliers to production/consumption rates.

**Engine requirement:** The engine just needs to fire `OnAdjacentTo`/`OnNotAdjacentTo` callbacks correctly. The Lua code handles the math. Our existing `SetProductionPerSecondMass/Energy` and `SetConsumptionPerSecondMass/Energy` methods are already implemented — FA's buff system calls these.

**So the only engine fix is 2.4 (OnAdjacentTo callback).**

### 2.6 Teleport Validation
**Problem:** `HasValidTeleportDest` returns false always (stub).

**Fix:** Implement real validation:
- Check destination is within playable rect
- Check destination terrain is passable (not deep water for land units)
- Check no other unit occupies the footprint at destination
- Return true if all checks pass

### 2.7 Death Weapon API
**Problem:** FA's unit scripts call `self:SetDeathWeaponEnabled(label, bool)` to enable/disable death weapons.

**Fix:** 
- Add `SetDeathWeaponEnabled(label, enabled)` — finds weapon by label, sets `fire_on_death` flag
- Add `GetDeathWeaponEnabled(label)` — returns current state
- Ensure `fire_on_death` weapons actually fire in the death sequence (check unit.cpp death path)

### 2.8 IssueKillSelf
**Problem:** No self-destruct command.

**Fix:** Add `IssueKillSelf(units)` in sim_bindings.cpp. For each unit, call `entity_Destroy` (same as killing it). FA uses this for ctrl+K self-destruct.

### 2.9 CreateVisibleAreaAtPoint (Eye of Rhianne / Scrying)
**What FA does:** Creates a temporary vision circle at a world position for a specific army. Used by Aeon ACU's Eye of Rhianne enhancement.

**Fix:**
- Add `CreateVisibleAreaAtPoint(army, x, y, z, radius, lifetime)` global function
- Creates a temporary entry in the visibility grid that reveals fog of war for the specified army at that position
- Auto-expires after `lifetime` seconds
- Store as a vector of `{army_idx, x, z, radius, remaining_ticks}` on SimState, tick down each sim tick

### 2.10 Air Staging Platforms
**What FA does:** Aircraft that land on staging platforms (T2 air staging) get refueled and repaired automatically.

**Implementation:**
- When an aircraft's fuel reaches 0 or it's damaged, the Lua AI code issues a repair/refuel command to the nearest staging platform
- The staging platform acts like a repair pad — the aircraft moves to it, and while "docked," fuel regenerates and HP repairs
- Most of this is handled in Lua (the unit script). The engine just needs:
  - `GetFuelRatio()` / `SetFuelRatio()` — already implemented
  - Repair while in range — already works via assist/repair system
  - The staging platform is a structure with `TransportClass` capability — transport load/unload may handle docking

**Likely already works** via existing transport/repair systems. Needs verification, not new code.

### 2.11 Engineering Drone Auto-Assist
**What FA does:** Drones created by `CreateUnitHPR` automatically guard their parent unit.

**Implementation:** This is handled entirely in Lua. The drone's OnCreate script issues `IssueGuard(self, parent)`. Our `IssueGuard` and `CreateUnitHPR` are both implemented. **Likely already works** — needs verification.

### 2.12 Quantum Gateway
**What FA does:** The Quantum Gateway is a structure that can build units (like a factory). It uses the standard factory production system with a different set of buildable blueprints.

**Implementation:** This uses the existing factory/construction system. The gateway's blueprint lists buildable units. `IssueBuildFactory` creates them. **Likely already works** — needs verification.

---

## Phase 3: Polish & Edge Cases

### 3.1 Loading Screen Animation
- `render_ui_only()` is called during reload but the loading dialog may not show the video background
- Verify `WldUIProvider:StartLoadingDialog()` triggers FA's `LoadDialog()` which creates a Movie control
- Ensure `pump_ui_frames()` is called between reload stages for smooth animation

### 3.2 Score Screen Display
- Verify score screen shows kills/losses/built/mass/energy stats
- Verify "Return to Lobby" button works (triggers `ReturnToLobby` → `__osc_return_to_lobby` flag)
- Verify lobby re-creates cleanly after return

### 3.3 Army Color Rendering
- Pass `PlayerColor`/`ArmyColor` from sessionConfig through to the renderer
- `ARMY_COLORS` array in renderer files should index by color from config, not hardcoded army index

### 3.4 Lobby Map Selection
- Verify the map preview updates when the host selects a different map
- Verify `DiskFindFiles` returns map list for the lobby's map browser

### 3.5 Multiple AI Support
- Verify 2+ AI armies can be added from the lobby
- Verify each AI gets its own personality and faction
- Verify spawn positions are honored per StartSpot

---

## Out of Scope
- Multiplayer networking (UDP, peer sync, replays)
- Campaign/mission system
- Mod support beyond what's already loaded
- Advanced AI threat analysis improvements
