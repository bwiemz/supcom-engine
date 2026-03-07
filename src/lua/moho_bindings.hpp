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

} // namespace osc::lua
