#pragma once

#include "core/types.hpp"

#include <vector>

struct lua_State;

namespace osc::sim {
class SimState;
class WorldHistory;
}

namespace osc::ui {
class UIControlRegistry;
}

namespace osc::lua {

class LuaState;

/// Advance the UI state's clock (what CurrentTime() returns) by one frame's
/// dt. Call once per UI frame: real dt in the window, a fixed step when
/// pumping frames headless.
void advance_ui_clock(lua_State* L, double dt);

/// Populate the `moho` global table with all class method tables.
/// Must be called before globalInit.lua executes.
void register_moho_bindings(LuaState& state, sim::SimState& sim);

/// Register UI global functions (InternalCreateGroup, InternalCreateFrame, etc.)
/// and store the UIControlRegistry pointer in Lua registry.
/// Must be called after register_moho_bindings.
void register_ui_bindings(LuaState& state, ui::UIControlRegistry& registry);

/// Register the LAN multiplayer UI globals (LanHost/LanJoin/LanNetStatus).
/// Called by register_ui_bindings; also usable standalone (e.g. --lan-ui-test).
void register_lan_ui_bindings(LuaState& state);

/// Register front-end bootstrap fallback globals that the FA UI import chain
/// expects, without replacing globals already installed by real bindings.
void register_front_end_fallback_bindings(LuaState& state);

/// Push an array of UI-side unit objects for the given entity ids (dead or
/// unknown ids are skipped), in the given order.
void push_units_for_ui(lua_State* L, const std::vector<osc::u32>& ids);

/// The ticks the renderer captures (a drawn game): the UI's unit objects
/// read the newest of them when it is the sim's current tick, rather than
/// capturing it again. Must outlive the UI state's use of it; null to stop.
void set_ui_world_source(lua_State* L, const sim::WorldHistory* history);

/// Once per sim beat: gamemain.OnFocusArmyUnitDamaged(unit) for each of the
/// focus army's units whose health dropped since the last call. Moho reports
/// damage to the player's own units this way; retail's music turns to
/// battle music on it (UserMusic.NotifyBattle). The previous health is kept
/// per sim, so a new game starts fresh.
void notify_focus_army_damage(lua_State* uiL, sim::SimState& sim);

} // namespace osc::lua
