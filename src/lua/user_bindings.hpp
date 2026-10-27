#pragma once

// The UI state's bindings that reach the game's view and input (M191): see
// user_bindings.cpp. Registered after register_moho_bindings and
// register_ui_bindings, before any UI script runs.

#include "core/types.hpp"
#include "sim/formation.hpp"

#include <string>
#include <vector>

struct lua_State;

namespace osc::sim {
class SimState;
}

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

/// The world views made before the window published its renderer (FA's
/// game interface, which a --map launch builds at boot), bound to its
/// camera as a new one is: their projections, picking and cartographic mode
/// reach it from then on.
void bind_unbound_world_views(lua_State* L);

/// The engine statistics for this frame (engine_stats.hpp), published as
/// __EngineStats as Moho's UI tick does: the frame's time and rate, the
/// process's memory, the game's entities by family, and with the cursor over
/// the WorldCamera's view, Camera > Cursor > Position
/// ("x=338.09,y=24.16,z=436.07"), Elevation (the ground under the cursor,
/// below any water), OCell ("x=338,z=436") and LODMetric, and the camera's
/// FocusDistance, in the forms of the dump of Moho's tree in FAF's
/// /lua/system/performance.lua. The Camera stats keep their last values while
/// the cursor is off the map or over another view; the minimap's (Minimap_*)
/// are not filled. `sim` is the game's, null in the front end.
void update_engine_stats(lua_State* L, f64 frame_seconds, const sim::SimState* sim);
/// The Cursor stats for the cursor on the world at `at` (GetMouseWorldPos: on
/// the water over a sea), as update_engine_stats writes them, published.
void update_cursor_stats(lua_State* L, const sim::Vector3& at);

/// plan_formation in the UI state, over its UserUnits (CFormation::Finalize)
std::vector<sim::FormationSlot> plan_ui_formation(lua_State* L,
                                                  const std::vector<sim::FormationMember>& units,
                                                  const map::Terrain* terrain,
                                                  const std::string& script, const sim::Vector3& at,
                                                  f32 facing);

/// The console commands that act on the game session (M217d, Moho's):
/// StartCommandMode, UI_SelectByCategory and IssueCommand.
void register_session_console_commands(ui::Console& console);

} // namespace osc::lua
