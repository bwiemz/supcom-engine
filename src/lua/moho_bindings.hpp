#pragma once

#include "core/types.hpp"
#include "sim/entity_id.hpp"

#include <vector>

struct lua_State;

namespace osc::sim {
class SimState;
class WorldHistory;
}

namespace osc::ui {
class Console;
class KeyMapRegistry;
class UIControlRegistry;
} // namespace osc::ui

namespace osc::lua {

class LuaState;

/// Advance the UI state's clock (what CurrentTime() returns) by one frame's
/// dt. Call once per UI frame: real dt in the window, a fixed step when
/// pumping frames headless.
void advance_ui_clock(lua_State* L, double dt);

/// Populate the `moho` global table with all class method tables.
/// Must be called before globalInit.lua executes.
void register_moho_bindings(LuaState& state, sim::SimState& sim);

/// The UI's WaitFor hands a table it is given (another binding's waitable:
/// the camera's move, M217g) to `hook`, which parks the thread or returns.
void set_ui_wait_hook(lua_State* L, int (*hook)(lua_State*));

/// Whether the UI state `L` has a world session, as Moho's user functions
/// ask WLD_GetActiveSession(): a game loading with its sim made, playing, or
/// at its score screen -- after SessionEndGame too, since Moho's session
/// stays active until the game is left. The front end has none.
bool session_active(lua_State* L);

/// The errors Moho's user functions raise with no session (faf-re Sim.cpp's
/// kNoSessionStartedText and kNoActiveSessionText): each raises its own.
inline constexpr const char* kNoSessionStarted = "No session started.";
inline constexpr const char* kNoActiveSession = "No active session";

/// Register UI global functions (InternalCreateGroup, InternalCreateFrame, etc.)
/// and store the UIControlRegistry pointer in Lua registry.
/// Must be called after register_moho_bindings.
void register_ui_bindings(LuaState& state, ui::UIControlRegistry& registry);

/// The engine's console commands (Moho's): UI_Lua, the selection sets, the
/// skin, layout and panel toggles, and the sim rate (WLD_GameSpeed,
/// WLD_IncreaseSimRate, WLD_DecreaseSimRate, WLD_ResetSimRate). Key map
/// actions and ConExecute run through them.
void register_console_commands(ui::Console& console);

/// Moho's IN_InitKeyHandler, in the UI state `L`: the key names from
/// keyNames.lua, and keymapper.lua's GetKeyMappings() into the key map.
void load_key_mappings(lua_State* L, ui::KeyMapRegistry& key_map);

/// Push an array of UI-side unit objects for the given entity ids (dead or
/// unknown ids are skipped), in the given order.
void push_units_for_ui(lua_State* L, const std::vector<sim::EntityId>& ids);

/// The ticks the renderer captures (a drawn game): the UI's unit objects
/// read the newest of them when it is the sim's current tick, rather than
/// capturing it again. Must outlive the UI state's use of it; null to stop.
void set_ui_world_source(lua_State* L, sim::WorldHistory* history);

/// Once per sim beat: gamemain.OnFocusArmyUnitDamaged(unit) for each of the
/// focus army's units whose health dropped since the last call. Moho reports
/// damage to the player's own units this way; retail's music turns to
/// battle music on it (UserMusic.NotifyBattle). The previous health is kept
/// per sim, so a new game starts fresh.
void notify_focus_army_damage(lua_State* uiL, sim::SimState& sim);

} // namespace osc::lua
