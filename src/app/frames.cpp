// The game UI's frames: the world UI's beats, command modes, selection,
// and the lobby's launch (M192 step 2, moved from app.cpp).

#include "app/app_internal.hpp"
#include "sim/collision.hpp"
#include "core/game_state.hpp"
#include "lua/game_mods.hpp"
#include "sim/lua_bytes.hpp"
#include "lua/lua_state.hpp"
#include "lua/script_loader.hpp"
#include "blueprints/blueprint_store.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/beat_system.hpp"
#include "ui/wld_ui_provider.hpp"
#include "renderer/renderer.hpp"
#include "ui/world_view.hpp"
#include "lua/sim_sync.hpp"
#include "sim/script_class.hpp"

namespace osc::app {

/// Hand the UI's queued SimCallbacks to the sim. Moho runs them as
/// commands, inside a tick: in multiplayer they are broadcast and every peer
/// runs them on the same tick; in single-player they run at the next tick.
void submit_sim_callbacks(osc::sim::SimCallbackQueue& queue, osc::sim::SimState& sim) {
    for (auto& cb : queue.drain()) sim.submit_callback(std::move(cb));
}

/// Whether the cursor is over FA's UI rather than the world: the deepest
/// hit-testable control under it is neither a WorldView (FA's world input
/// surface) nor the root frame.
bool mouse_over_ui(lua_State* uiL, osc::f64 x, osc::f64 y) {
    lua_pushstring(uiL, "osc_ui_registry");
    lua_rawget(uiL, LUA_REGISTRYINDEX);
    auto* registry = static_cast<osc::ui::UIControlRegistry*>(lua_touserdata(uiL, -1));
    lua_pop(uiL, 1);
    if (!registry) return false;
    osc::ui::UIDispatch dispatch;
    return dispatch.ui_has_mouse(uiL, *registry, x, y);
}

// ── FA's command mode (/lua/ui/game/commandmode.lua) ──
static constexpr const char* kCommandModeModule = "/lua/ui/game/commandmode.lua";

/// A unit blueprint's footprint from the UI state's blueprint store.
static void blueprint_footprint(lua_State* uiL, const std::string& bp_id, osc::f32& sx,
                                osc::f32& sz) {
    auto* store = osc::lua::LuaState::get_blueprint_store(uiL);
    auto* entry = store ? store->find(bp_id) : nullptr;
    if (!entry) return;
    store->push_lua_table(*entry, uiL);
    const auto [fx, fz] = osc::sim::blueprint_footprint(uiL, lua_gettop(uiL));
    if (fx > 0) {
        sx = fx;
    }
    if (fz > 0) {
        sz = fz;
    }
    lua_pop(uiL, 1);
}

/// FA's current command mode: GetCommandMode() -> {mode, data}, once the
/// game UI has loaded the module. Build mode carries the footprint (for
/// the ghost and the snap).
/// An ability's order at a point (M206w): the command mode's table as it
/// is now, with the point as its Location, as bytes. Moho's click gives the
/// task its target so (TargetLocation reads commandData.Location).
static std::function<std::string(const osc::sim::Vector3&)> script_order_at(lua_State* uiL) {
    std::string mode_data;
    osc::core::push_loaded_module_function(uiL, kCommandModeModule, "GetCommandMode");
    if (lua_isfunction(uiL, -1) && lua_pcall(uiL, 0, 1, 0) == 0 && lua_istable(uiL, -1)) {
        lua_rawgeti(uiL, -1, 2);
        if (lua_istable(uiL, -1))
            if (auto bytes = osc::sim::lua_to_bytes(uiL, -1)) mode_data = std::move(*bytes);
        lua_pop(uiL, 1);
    }
    lua_pop(uiL, 1); // the mode, the error or the non-function
    return [uiL, mode_data](const osc::sim::Vector3& at) -> std::string {
        const int top = lua_gettop(uiL);
        if (!osc::sim::push_lua_bytes(uiL, mode_data) || !lua_istable(uiL, -1)) {
            lua_settop(uiL, top);
            return {};
        }
        lua_pushstring(uiL, "Location");
        lua_newtable(uiL);
        const float xyz[] = {at.x, at.y, at.z};
        for (int i = 0; i < 3; ++i) {
            lua_pushnumber(uiL, xyz[i]);
            lua_rawseti(uiL, -2, i + 1);
        }
        lua_rawset(uiL, -3);
        auto bytes = osc::sim::lua_to_bytes(uiL, -1);
        lua_settop(uiL, top);
        return bytes ? std::move(*bytes) : std::string();
    };
}

osc::renderer::CommandMode read_command_mode(lua_State* uiL) {
    osc::renderer::CommandMode m;
    osc::core::push_loaded_module_function(uiL, kCommandModeModule, "GetCommandMode");
    if (!lua_isfunction(uiL, -1)) {
        lua_pop(uiL, 1);
        return m;
    }
    if (lua_pcall(uiL, 0, 1, 0) != 0) {
        spdlog::warn("GetCommandMode error: {}", lua_tostring(uiL, -1));
        lua_pop(uiL, 1);
        return m;
    }
    if (lua_istable(uiL, -1)) {
        lua_rawgeti(uiL, -1, 1);
        if (lua_type(uiL, -1) == LUA_TSTRING) m.mode = lua_tostring(uiL, -1);
        lua_pop(uiL, 1);
        lua_rawgeti(uiL, -1, 2);
        if (lua_istable(uiL, -1)) {
            lua_pushstring(uiL, "name");
            lua_rawget(uiL, -2);
            if (lua_type(uiL, -1) == LUA_TSTRING) m.name = lua_tostring(uiL, -1);
            lua_pop(uiL, 1);
        }
        lua_pop(uiL, 1);
    }
    lua_pop(uiL, 1);
    if (m.mode == "build" && !m.name.empty())
        blueprint_footprint(uiL, m.name, m.footprint_x, m.footprint_z);
    if (m.mode == "order" && m.name == "RULEUCC_Script") m.script_args_at = script_order_at(uiL);
    return m;
}

/// Report an issued command to commandmode.OnCommandIssued, as Moho does:
/// a non-Shift command ends the mode, and FA draws its feedback blip.
void report_command_issued(lua_State* uiL, const osc::renderer::IssuedCommand& c) {
    lua_newtable(uiL);
    lua_pushstring(uiL, "CommandType");
    lua_pushstring(uiL, c.type.c_str());
    lua_rawset(uiL, -3);
    lua_pushstring(uiL, "Clear");
    lua_pushboolean(uiL, c.clear ? 1 : 0);
    lua_rawset(uiL, -3);
    if (!c.blueprint.empty()) {
        lua_pushstring(uiL, "Blueprint");
        lua_pushstring(uiL, c.blueprint.c_str());
        lua_rawset(uiL, -3);
    }
    lua_pushstring(uiL, "Target");
    lua_newtable(uiL);
    lua_pushstring(uiL, "Type");
    lua_pushstring(uiL, c.target_id ? "Entity" : "Position");
    lua_rawset(uiL, -3);
    if (c.target_id) {
        lua_pushstring(uiL, "EntityId");
        osc::sim::push_entity_id(uiL, c.target_id);
        lua_rawset(uiL, -3);
    }
    lua_pushstring(uiL, "Position");
    lua_newtable(uiL);
    lua_pushnumber(uiL, c.position.x);
    lua_rawseti(uiL, -2, 1);
    lua_pushnumber(uiL, c.position.y);
    lua_rawseti(uiL, -2, 2);
    lua_pushnumber(uiL, c.position.z);
    lua_rawseti(uiL, -2, 3);
    lua_rawset(uiL, -3); // Target.Position
    lua_rawset(uiL, -3); // command.Target
    osc::core::call_ui_callback(uiL, kCommandModeModule, "OnCommandIssued", 1);
}

/// Leave FA's command mode, cancelled (a right-click).
void cancel_command_mode(lua_State* uiL) {
    lua_pushboolean(uiL, 1);
    osc::core::call_ui_callback(uiL, kCommandModeModule, "EndCommandMode", 1);
}

/// Show the build ghost while FA is in build mode. The ghost is cleared
/// only if this set it (other code may place ghosts too).
void sync_build_ghost(osc::sim::SimState& sim, const osc::renderer::CommandMode& m,
                      bool& ghost_from_mode) {
    if (m.mode == "build" && !m.name.empty()) {
        sim.set_build_ghost(m.name, m.footprint_x, m.footprint_z);
        ghost_from_mode = true;
    } else if (ghost_from_mode) {
        sim.clear_build_ghost();
        ghost_from_mode = false;
    }
}

/// A selection action reaches the UI as Moho reports it,
/// gamemain.OnSelectionChanged(old, new, added, removed), and then the
/// engine's own AddOnSelectionChangedCallback callbacks. Moho reports every
/// action (`action`), even one that leaves the selection unchanged -- retail
/// refreshes the orders and construction panels on those -- as well as any
/// change. `prev` becomes `cur`.
void dispatch_selection_change(lua_State* uL, std::unordered_set<osc::u32>& prev,
                               const std::unordered_set<osc::u32>& cur, bool action) {
    if (cur == prev && !action) return;
    std::vector<osc::u32> old_ids(prev.begin(), prev.end());
    std::vector<osc::u32> new_ids(cur.begin(), cur.end());
    std::sort(old_ids.begin(), old_ids.end());
    std::sort(new_ids.begin(), new_ids.end());
    std::vector<osc::u32> added, removed;
    std::set_difference(new_ids.begin(), new_ids.end(), old_ids.begin(),
                        old_ids.end(), std::back_inserter(added));
    std::set_difference(old_ids.begin(), old_ids.end(), new_ids.begin(),
                        new_ids.end(), std::back_inserter(removed));
    prev = cur;

    osc::lua::push_units_for_ui(uL, old_ids);
    osc::lua::push_units_for_ui(uL, new_ids);
    osc::lua::push_units_for_ui(uL, added);
    osc::lua::push_units_for_ui(uL, removed);
    osc::core::call_ui_callback(uL, osc::core::kGameMainModule,
                                "OnSelectionChanged", 4);

    lua_pushstring(uL, "__osc_sel_changed_cbs");
    lua_rawget(uL, LUA_REGISTRYINDEX);
    if (lua_istable(uL, -1)) {
        const int cbs_idx = lua_gettop(uL);
        osc::lua::push_units_for_ui(uL, new_ids);
        const int arr_idx = lua_gettop(uL);
        const int n = luaL_getn(uL, cbs_idx); // Lua 5.0: no lua_objlen
        for (int ci = 1; ci <= n; ci++) {
            lua_rawgeti(uL, cbs_idx, ci);
            if (!lua_isfunction(uL, -1)) {
                lua_pop(uL, 1);
                continue;
            }
            lua_pushvalue(uL, arr_idx);
            if (lua_pcall(uL, 1, 0, 0) != 0) {
                const char* err = lua_tostring(uL, -1);
                spdlog::warn("OnSelectionChanged[{}] error: {}", ci,
                             err ? err : "(unknown)");
                lua_pop(uL, 1);
            }
        }
        lua_pop(uL, 1); // selection array
    }
    lua_pop(uL, 1); // callbacks table (or nil)
}

/// Moho's world-UI start (see ui::WldUIProvider): the user side of the
/// sync channel (/lua/UserSync.lua and its hooks: OnSync, a fresh Sync and
/// UnitData) is loaded for the new session -- by its SessionInit.lua, when
/// the state ran that -- then uimain.StartGameUI makes the Lua provider,
/// whose loading dialog shows while the world loads.
void begin_world_ui(lua_State* uiL, osc::ui::WldUIProvider& wld) {
    if (!osc::lua::session_init_ran(uiL)) {
        if (auto r = osc::lua::run_vfs_script(uiL, "/lua/UserSync.lua"); !r)
            spdlog::warn("UserSync.lua: {}", r.error().message);
    }
    osc::core::call_start_game_ui(uiL);
    wld.start_loading_dialog(uiL);
}

/// One Moho sim beat on the user side: the sync channel (sim -> UI data and
/// focus changes, OnSync), then the game UI's beat functions.
void world_beat(osc::lua::LuaState* sim_lua, osc::sim::SimState* sim, lua_State* uiL) {
    if (sim_lua) osc::lua::sync_beat(sim_lua->raw(), uiL);
    if (sim) osc::lua::notify_focus_army_damage(uiL, *sim);
    osc::core::call_game_beat(uiL);
}

/// Game over, as Moho reports it: once the sim has ended the session
/// (EndGame from the scenario's victory script, or the engine's own
/// adjudication without one), the UI hears uimain.NoteGameOver. Retail's
/// game-result UI (Sync.GameResult -> DoGameResult) announces the outcome and
/// offers the score screen: nothing is paused and the view is not switched.
void note_game_over_if_ended(osc::sim::SimState* sim, osc::GameStateManager& mgr, lua_State* uiL) {
    if (!sim || !sim->game_ended() || mgr.game_over()) return;
    mgr.set_game_over(true);
    const osc::i32 result = sim->player_result();
    spdlog::info("Game over: {}", result == 1   ? "VICTORY"
                                  : result == 2 ? "DEFEAT"
                                  : result == 3 ? "DRAW"
                                                : "ended");
    osc::core::call_note_game_over(uiL);
}

/// The world is loaded, as Moho's WLD_DoInitializing hands over: the sim's
/// first sync reaches the UI (its OnSync: a campaign's Sync.CampaignMode,
/// set as its armies are made), the loading dialog stops, then FA's game
/// interface is built (gamemain.CreateUI), which reads what that sync
/// brought: a campaign's interface stays up for its intro's NIS to hide, a
/// skirmish's hides until the loading screen's fade slides it in.
void finish_world_ui(lua_State* uiL, osc::ui::WldUIProvider& wld, bool is_replay,
                     osc::lua::LuaState* sim_lua, osc::sim::SimState* sim) {
    world_beat(sim_lua, sim, uiL);
    wld.stop_loading_dialog(uiL);
    wld.create_game_interface(uiL, is_replay);
}

/// Pump N UI frames: resume coroutines, fire OnBeat, fire beat functions.
void pump_ui_frames(osc::lua::LuaState& ui_lua_state, osc::sim::ThreadManager& ui_thread_manager,
                    osc::lua::BeatFunctionRegistry& beat_registry, int count,
                    osc::u32& ui_frame_counter) {
    lua_State* uL = ui_lua_state.raw();
    for (int i = 0; i < count; i++) {
        ui_frame_counter++;
        osc::lua::advance_ui_clock(uL, 1.0 / 60.0);
        ui_thread_manager.resume_all(ui_frame_counter);
        osc::core::call_on_beat(uL, 1.0 / 30.0);
        beat_registry.fire_all(uL);
    }
}

void pump_ui_frames_with_controls(osc::lua::LuaState& ui_lua_state,
                                  osc::sim::ThreadManager& ui_thread_manager,
                                  osc::lua::BeatFunctionRegistry& beat_registry,
                                  osc::ui::UIControlRegistry& ui_registry, int count,
                                  osc::u32& ui_frame_counter) {
    lua_State* uL = ui_lua_state.raw();
    osc::ui::UIDispatch dispatch;
    for (int i = 0; i < count; i++) {
        ui_frame_counter++;
        osc::lua::advance_ui_clock(uL, 1.0 / 60.0);
        ui_thread_manager.resume_all(ui_frame_counter);
        dispatch.update_controls(uL, ui_registry, 1.0 / 60.0);
        dispatch.dispatch_events(uL, ui_registry);
        osc::core::call_on_beat(uL, 1.0 / 30.0);
        beat_registry.fire_all(uL);
    }
}

} // namespace osc::app
