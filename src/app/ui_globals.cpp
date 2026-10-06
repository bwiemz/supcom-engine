// The session's UI Lua globals: what the game's UI asks of the session
// (M192 step 2, moved from app.cpp).

#include "app/app_internal.hpp"
#include "lua/lobby_wire.hpp"
#include "lua/lua_state.hpp"
#include "lua/mp_net_state.hpp"
#include "map/terrain.hpp"

extern "C" {
#include <lua.h>
}

#include <utility>
#include <vector>

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

/// Push the sim's ScenarioInfo[`key`] (a lobby's GameOptions for Options, as
/// Moho's are), copied into `L`; false, pushing nothing, without one.
static bool push_sim_scenario_field(lua_State* L, const osc::sim::SimState& sim, const char* key) {
    lua_State* S = sim.lua_state();
    std::vector<osc::u8> bytes;
    if (S) {
        const int top = lua_gettop(S);
        lua_pushstring(S, "ScenarioInfo"); // raw: the sim's globals are strict
        lua_rawget(S, LUA_GLOBALSINDEX);
        if (lua_istable(S, -1)) {
            lua_pushstring(S, key);
            lua_rawget(S, -2);
            if (!lua_isnil(S, -1)) bytes = osc::lua::encode_lobby_value(S, -1);
        }
        lua_settop(S, top);
    }
    if (bytes.empty()) return false;
    if (osc::lua::push_lobby_value(L, bytes)) return true;
    lua_pop(L, 1); // the nil a malformed copy pushes
    return false;
}

/// Registry keys of the UI state's ScenarioInfo and the game it is for.
constexpr const char* kScenarioInfoKey = "__osc_session_scenario_info";
constexpr const char* kScenarioInfoGenKey = "__osc_session_scenario_info_gen";

/// SessionGetScenarioInfo() for the UI: the session's ScenarioInfo, one
/// table for the game as Moho's is -- FAF's UserSync writes the playable
/// area into it, which its score board reads from the table it took at load.
static int l_SessionGetScenarioInfo(lua_State* L) {
    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* sim = static_cast<osc::sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    if (!sim || !sim->terrain()) {
        lua_newtable(L);
        return 1;
    }
    const auto generation = static_cast<lua_Number>(osc::sim::SimState::sim_generation());
    lua_pushstring(L, kScenarioInfoGenKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const bool current = lua_isnumber(L, -1) && lua_tonumber(L, -1) == generation;
    lua_pop(L, 1);
    if (current) {
        lua_pushstring(L, kScenarioInfoKey);
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (lua_istable(L, -1)) return 1;
        lua_pop(L, 1);
    }

    lua_newtable(L);

    // The scenario file's own fields, where the sim has them.
    for (const auto& [key, fallback] : {std::pair{"name", "Skirmish"}, std::pair{"map", ""}}) {
        lua_pushstring(L, key);
        bool text = push_sim_scenario_field(L, *sim, key);
        if (text && lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 1);
            text = false;
        }
        if (!text) lua_pushstring(L, fallback);
        lua_rawset(L, -3);
    }
    for (const char* key : {"description", "type", "preview", "save", "script", "norushradius",
                            "starts", "map_version"}) {
        lua_pushstring(L, key);
        if (push_sim_scenario_field(L, *sim, key)) lua_rawset(L, -3);
        else lua_pop(L, 1);
    }

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

    // Its options: retail's tabs reads Options.Timeouts in a network game
    lua_pushstring(L, "Options");
    bool options = push_sim_scenario_field(L, *sim, "Options");
    if (options && !lua_istable(L, -1)) {
        lua_pop(L, 1);
        options = false;
    }
    if (!options) lua_newtable(L);
    lua_rawset(L, -3);

    lua_pushstring(L, kScenarioInfoKey);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, kScenarioInfoGenKey);
    lua_pushnumber(L, generation);
    lua_rawset(L, LUA_REGISTRYINDEX);
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

    // The rates are a tick's worth, as Moho's: retail's economy bar
    // multiplies them by GetSimTicksPerSecond.
    constexpr osc::f64 kPerTick = osc::sim::SimState::SECONDS_PER_TICK;
    push_resource_subtable("income", econ.mass.income * kPerTick, econ.energy.income * kPerTick);
    push_resource_subtable("lastUseActual",
                           brain ? brain->get_economy_usage("MASS") * kPerTick : 0.0,
                           brain ? brain->get_economy_usage("ENERGY") * kPerTick : 0.0);
    push_resource_subtable("lastUseRequested", econ.mass.requested * kPerTick,
                           econ.energy.requested * kPerTick);
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

            // 0-based (UEF 0), as the UI indexes factions.lua's list with
            // faction + 1; the brain's is the sim's 1-based GetFactionIndex
            lua_pushstring(L, "faction");
            lua_pushnumber(L, brain->faction() - 1);
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

            // The command sources that play it (1-based), as chat.lua finds
            // an army's clients by them: a network game's sources of the
            // army, a single-player game's one player
            lua_pushstring(L, "authorizedCommandSources");
            lua_newtable(L);
            {
                const auto& mp = osc::lua::mp_net_state();
                int n = 0;
                if (mp.active() || !mp.clients.empty()) {
                    for (const osc::u32 source : mp.all_sources)
                        if (mp.army_of(source) == static_cast<int>(i)) {
                            lua_pushnumber(L, static_cast<double>(source + 1));
                            lua_rawseti(L, -2, ++n);
                        }
                } else if (brain->is_human() && !brain->is_civilian()) {
                    lua_pushnumber(L, 1);
                    lua_rawseti(L, -2, ++n);
                }
            }
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
