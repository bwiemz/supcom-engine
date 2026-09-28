#pragma once

// The UI state's bindings that reach the game's view and input (M191): see
// user_bindings.cpp. Registered after register_moho_bindings and
// register_ui_bindings, before any UI script runs.

namespace osc::ui {
class Console;
}

namespace osc::lua {

class LuaState;

void register_user_bindings(LuaState& state);

/// The console commands that act on the game session (M217d, Moho's):
/// StartCommandMode, UI_SelectByCategory and IssueCommand.
void register_session_console_commands(ui::Console& console);

} // namespace osc::lua
