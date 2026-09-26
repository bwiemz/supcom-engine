// The session's UI Lua globals: what the game's UI asks of the session
// (M192 step 2, moved from app.cpp).

#include "app/app_internal.hpp"
#include "lua/lua_state.hpp"
#include "map/terrain.hpp"

extern "C" {
#include <lua.h>
}

namespace osc::app {

// ── Engine global Lua C functions (file-static, outside run) ──
// Note: GetCurrentUIState and WorldIsLoading moved to moho_bindings.cpp (M144c)

static int l_FlushEvents(lua_State*) { return 0; }

/// SessionIsReplay() for the UI: whether the game is a replay being played
/// (retail's UI then hides orders and shows the replay controls). Only UI
/// scripts ask it; the sim's own answer stays false, so a replay can't
/// change what the game does. A saved game catching up plays back too, but
/// it is the player's game.
static int l_SessionIsReplay(lua_State* L) {
    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    const auto* sim = static_cast<const osc::sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    lua_pushboolean(L, sim && sim->playback() && !sim->resuming() ? 1 : 0);
    return 1;
}

static int l_SessionGetScenarioInfo(lua_State* L) {
    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* sim = static_cast<osc::sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    lua_newtable(L);
    if (!sim || !sim->terrain()) return 1;

    lua_pushstring(L, "name");
    lua_pushstring(L, "Skirmish");
    lua_rawset(L, -3);

    lua_pushstring(L, "map");
    lua_pushstring(L, "");
    lua_rawset(L, -3);

    // {width, height}, as a scenario file gives it (retail indexes size[1],
    // size[2]: the world border, the map's km in the lobby's info).
    lua_pushstring(L, "size");
    lua_newtable(L);
    lua_pushnumber(L, sim->terrain()->map_width());
    lua_rawseti(L, -2, 1);
    lua_pushnumber(L, sim->terrain()->map_height());
    lua_rawseti(L, -2, 2);
    lua_rawset(L, -3);

    lua_pushstring(L, "PlayableArea");
    lua_newtable(L);
    lua_pushnumber(L, 0); lua_rawseti(L, -2, 1);
    lua_pushnumber(L, 0); lua_rawseti(L, -2, 2);
    lua_pushnumber(L, sim->terrain()->map_width()); lua_rawseti(L, -2, 3);
    lua_pushnumber(L, sim->terrain()->map_height()); lua_rawseti(L, -2, 4);
    lua_rawset(L, -3);

    return 1;
}

static int l_GetEconomyTotals(lua_State* L) {
    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* sim = static_cast<osc::sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    lua_pushstring(L, "__osc_focus_army");
    lua_rawget(L, LUA_REGISTRYINDEX);
    int army = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : 0;
    lua_pop(L, 1);

    lua_newtable(L); // result table
    // Observers (focus army -1) and a missing sim get the same shape with
    // zeros, as Moho's does: the economy bar reads every field every beat.
    auto* brain = sim ? sim->get_army(army) : nullptr;
    static const osc::sim::EconomyState kNoEconomy{};
    const auto& econ = brain ? brain->economy() : kNoEconomy;

    // Helper: push a subtable with MASS and ENERGY keys
    auto push_resource_subtable = [&](const char* name, osc::f64 mass_val, osc::f64 energy_val) {
        lua_pushstring(L, name);
        lua_newtable(L);
        lua_pushstring(L, "MASS");
        lua_pushnumber(L, mass_val);
        lua_rawset(L, -3);
        lua_pushstring(L, "ENERGY");
        lua_pushnumber(L, energy_val);
        lua_rawset(L, -3);
        lua_rawset(L, -3); // set subtable on result
    };

    push_resource_subtable("income", econ.mass.income, econ.energy.income);
    push_resource_subtable("lastUseActual",
        brain ? brain->get_economy_usage("MASS") : 0.0,
        brain ? brain->get_economy_usage("ENERGY") : 0.0);
    push_resource_subtable("lastUseRequested", econ.mass.requested, econ.energy.requested);
    push_resource_subtable("maxStorage", econ.mass.max_storage, econ.energy.max_storage);
    push_resource_subtable("stored", econ.mass.stored, econ.energy.stored);
    push_resource_subtable("reclaimed", 0.0, 0.0); // TODO: track cumulative reclaim

    return 1;
}

static int l_GetSimTicksPerSecond(lua_State* L) {
    lua_pushnumber(L, 10.0);
    return 1;
}

static int l_ui_IsAlly(lua_State* L) {
    int army1 = static_cast<int>(lua_tonumber(L, 1)) - 1; // 1-based Lua → 0-based C++
    int army2 = static_cast<int>(lua_tonumber(L, 2)) - 1;

    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* sim = static_cast<osc::sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    if (!sim) { lua_pushboolean(L, 0); return 1; }
    auto* brain = sim->get_army(army1);
    if (!brain) { lua_pushboolean(L, 0); return 1; }

    lua_pushboolean(L, brain->is_ally(army2) ? 1 : 0);
    return 1;
}

static int l_GetArmiesTable(lua_State* L) {
    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* sim = static_cast<osc::sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    lua_newtable(L); // result table

    // Build the armiesTable array
    lua_pushstring(L, "armiesTable");
    lua_newtable(L); // armiesTable array

    if (sim) {
        for (size_t i = 0; i < sim->army_count(); ++i) {
            auto* brain = sim->army_at(i);
            if (!brain) continue;

            lua_newtable(L); // per-army entry

            lua_pushstring(L, "nickname");
            lua_pushstring(L, brain->nickname().c_str());
            lua_rawset(L, -3);

            lua_pushstring(L, "ArmyName");
            lua_pushstring(L, brain->name().c_str());
            lua_rawset(L, -3);

            lua_pushstring(L, "armyIndex");
            lua_pushnumber(L, static_cast<int>(i));
            lua_rawset(L, -3);

            lua_pushstring(L, "human");
            lua_pushboolean(L, brain->is_human() ? 1 : 0);
            lua_rawset(L, -3);

            lua_pushstring(L, "civilian");
            lua_pushboolean(L, brain->is_civilian() ? 1 : 0);
            lua_rawset(L, -3);

            lua_pushstring(L, "outOfGame");
            lua_pushboolean(L, brain->is_defeated() ? 1 : 0);
            lua_rawset(L, -3);

            lua_pushstring(L, "faction");
            lua_pushnumber(L, brain->faction());
            lua_rawset(L, -3);

            // Color as ARGB hex string (e.g. "ffFF8000")
            {
                char color_buf[16];
                std::snprintf(color_buf, sizeof(color_buf), "ff%02X%02X%02X",
                    brain->color_r(), brain->color_g(), brain->color_b());
                lua_pushstring(L, "color");
                lua_pushstring(L, color_buf);
                lua_rawset(L, -3);
            }

            lua_pushstring(L, "showScore");
            lua_pushboolean(L, brain->is_civilian() ? 0 : 1);
            lua_rawset(L, -3);

            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
    }

    lua_rawset(L, -3); // result.armiesTable = array

    // focusArmy field
    lua_pushstring(L, "focusArmy");
    lua_pushstring(L, "__osc_focus_army");
    lua_rawget(L, LUA_REGISTRYINDEX);
    int focus = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : 0;
    lua_pop(L, 1);
    lua_pushnumber(L, focus + 1); // 1-based for Lua
    lua_rawset(L, -3);

    // numArmies field
    lua_pushstring(L, "numArmies");
    lua_pushnumber(L, sim ? static_cast<int>(sim->army_count()) : 0);
    lua_rawset(L, -3);

    return 1;
}

/// The session's UI globals above, on the UI state.
void register_session_ui_globals(osc::lua::LuaState& ui_lua_state) {
    ui_lua_state.register_function("FlushEvents", l_FlushEvents);
    ui_lua_state.register_function("SessionIsReplay", l_SessionIsReplay);
    ui_lua_state.register_function("SessionGetScenarioInfo", l_SessionGetScenarioInfo);
    ui_lua_state.register_function("GetEconomyTotals", l_GetEconomyTotals);
    ui_lua_state.register_function("GetSimTicksPerSecond", l_GetSimTicksPerSecond);
    ui_lua_state.register_function("GetArmiesTable", l_GetArmiesTable);
    ui_lua_state.register_function("IsAlly", l_ui_IsAlly);
}

} // namespace osc::app
