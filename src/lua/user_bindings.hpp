#pragma once

// The UI state's bindings that reach the game's view and input (M191): see
// user_bindings.cpp. Registered after register_moho_bindings and
// register_ui_bindings, before any UI script runs.

namespace osc::lua {

class LuaState;

void register_user_bindings(LuaState& state);

} // namespace osc::lua
