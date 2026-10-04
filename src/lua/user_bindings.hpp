#pragma once

// The UI state's bindings that reach the game's view and input (M191): see
// user_bindings.cpp. Registered after register_moho_bindings and
// register_ui_bindings, before any UI script runs.

struct lua_State;

namespace osc::ui {
class Console;
}

namespace osc::lua {

class LuaState;

void register_user_bindings(LuaState& state);

/// The main world view's OnUpdateCursor, as Moho's CUIWorldView calls it each
/// frame the cursor is over it: retail's worldview.lua picks the cursor from
/// the command mode and GetRightMouseButtonOrder
void update_world_view_cursor(lua_State* L);

/// The console commands that act on the game session (M217d, Moho's):
/// StartCommandMode, UI_SelectByCategory and IssueCommand.
void register_session_console_commands(ui::Console& console);

} // namespace osc::lua
