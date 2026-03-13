#pragma once

struct lua_State;

namespace osc::sim {
class SimState;
}

namespace osc::ui {
class UIControlRegistry;
}

namespace osc::lua {

class LuaState;

/// Populate the `moho` global table with all class method tables.
/// Must be called before globalInit.lua executes.
void register_moho_bindings(LuaState& state, sim::SimState& sim);

/// Register UI global functions (InternalCreateGroup, InternalCreateFrame, etc.)
/// and store the UIControlRegistry pointer in Lua registry.
/// Must be called after register_moho_bindings.
void register_ui_bindings(LuaState& state, ui::UIControlRegistry& registry);

/// Push the current selection as a Lua array table onto the UI Lua state stack.
/// Each element is a unit table with _c_object, EntityId, Army, and the
/// __osc_ui_unit_mt metatable (same as GetSelectedUnits()).
/// Pushes exactly 1 value.
void push_selected_units_for_ui(lua_State* L);

} // namespace osc::lua
