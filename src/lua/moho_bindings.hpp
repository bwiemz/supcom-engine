#pragma once

#include "core/types.hpp"

#include <vector>

struct lua_State;

namespace osc::sim {
class SimState;
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

/// Push the current selection as a Lua array table onto the UI Lua state stack.
/// Each element is a unit table with _c_object, EntityId, Army, and the
/// __osc_ui_unit_mt metatable (same as GetSelectedUnits()).
/// Pushes exactly 1 value.
void push_selected_units_for_ui(lua_State* L);
/// Push an array of UI-side unit objects for the given entity ids (dead or
/// unknown ids are skipped), in the given order.
void push_units_for_ui(lua_State* L, const std::vector<osc::u32>& ids);

} // namespace osc::lua
