#include "lua/session_manager.hpp"
#include "lua/lua_state.hpp"
#include "sim/army_brain.hpp"
#include "sim/sim_state.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::lua {

Result<void> SessionManager::start_session(LuaState& state,
                                            const vfs::VirtualFileSystem&,
                                            sim::SimState& sim,
                                            const ScenarioMetadata& meta) {
    spdlog::info("Starting session...");
    lua_State* L = state.raw();

    // Step 1: Populate ScenarioInfo.ArmySetup for Lua code
    setup_army_info(L, meta);

    // Step 2: Call SetupSession() — loads save + script files
    spdlog::info("  Calling SetupSession()...");
    auto setup_result = call_setup_session(L);
    if (!setup_result) {
        spdlog::warn("  SetupSession() failed: {} (continuing anyway)",
                      setup_result.error().message);
    }

    // Step 3: Extract start positions from Scenario.MasterChain markers
    extract_start_positions(L, sim);

    // Step 4: Create army brains
    spdlog::info("  Creating army brains ({} armies)...", meta.armies.size());
    i32 brains_created = 0;
    for (size_t i = 0; i < meta.armies.size(); i++) {
        auto result = create_army_brain(L, sim, static_cast<i32>(i),
                                         meta.armies[i], meta.armies[i]);
        if (!result) {
            spdlog::warn("  Failed to create brain for {}: {}",
                          meta.armies[i], result.error().message);
        } else {
            brains_created++;
        }
    }

    if (brains_created == 0 && !meta.armies.empty()) {
        return Error("All army brain creations failed");
    }

    // Step 5: Call BeginSession()
    spdlog::info("  Calling BeginSession()...");
    auto begin_result = call_begin_session(L);
    if (!begin_result) {
        spdlog::warn("  BeginSession() failed: {} (continuing anyway)",
                      begin_result.error().message);
    }

    session_active_ = true;

    // Set session_active flag in Lua registry for SessionIsActive()
    lua_pushstring(L, "osc_session_active");
    lua_pushboolean(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);

    spdlog::info("Session started.");
    return {};
}

void SessionManager::setup_army_info(lua_State* L,
                                      const ScenarioMetadata& meta) {
    // Push ScenarioInfo onto stack
    lua_getglobal(L, "ScenarioInfo");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    int si_idx = lua_gettop(L);

    // Create ScenarioInfo.Options (required by SetupSession)
    lua_pushstring(L, "Options");
    lua_newtable(L);
    int opts_idx = lua_gettop(L);

    // Populate default game options
    auto set_opt_str = [L, opts_idx](const char* key, const char* val) {
        lua_pushstring(L, key);
        lua_pushstring(L, val);
        lua_rawset(L, opts_idx);
    };
    auto set_opt_num = [L, opts_idx](const char* key, f64 val) {
        lua_pushstring(L, key);
        lua_pushnumber(L, val);
        lua_rawset(L, opts_idx);
    };

    set_opt_str("Share", "ShareUntilDeath");
    set_opt_str("TeamSpawn", "fixed");
    set_opt_str("TeamLock", "locked");
    set_opt_str("Victory", "demoralization");
    set_opt_num("Timeouts", 3);
    set_opt_num("UnitCap", 1000);
    set_opt_num("GameSpeed", 0);         // normal
    set_opt_str("FogOfWar", "explored");
    set_opt_str("CheatsEnabled", "false");
    set_opt_str("PrebuiltUnits", "Off");
    set_opt_str("CivilianAlliance", "enemy");
    set_opt_str("RevealCivilians", "Yes");
    set_opt_str("NoRushOption", "Off");
    set_opt_num("Score", 1);
    set_opt_str("ShareUnitCap", "none");
    set_opt_str("TeamShareOverflow", "none");
    set_opt_str("CommonArmy", "Off");

    // RestrictedCategories = {} (empty table)
    lua_pushstring(L, "RestrictedCategories");
    lua_newtable(L);
    lua_rawset(L, opts_idx);

    lua_rawset(L, si_idx); // ScenarioInfo.Options = opts table

    // Create ScenarioInfo.ArmySetup = {}
    lua_pushstring(L, "ArmySetup");
    lua_newtable(L);
    int setup_idx = lua_gettop(L);

    for (size_t i = 0; i < meta.armies.size(); i++) {
        const auto& army_name = meta.armies[i];
        lua_pushstring(L, army_name.c_str());
        lua_newtable(L);

        // Civilian check (must be before Human flag)
        bool is_civilian = army_name.find("CIVILIAN") != std::string::npos ||
                           army_name.find("NEUTRAL") != std::string::npos;
        lua_pushstring(L, "Civilian");
        lua_pushboolean(L, is_civilian ? 1 : 0);
        lua_rawset(L, -3);

        // Human flag — non-civilian armies are human unless marked as AI
        bool is_ai = !is_civilian && is_ai_army(static_cast<int>(i));
        lua_pushstring(L, "Human");
        lua_pushboolean(L, (is_civilian || is_ai) ? 0 : 1);
        lua_rawset(L, -3);

        // ArmyName
        lua_pushstring(L, "ArmyName");
        lua_pushstring(L, army_name.c_str());
        lua_rawset(L, -3);

        // Nickname
        lua_pushstring(L, "Nickname");
        lua_pushstring(L, army_name.c_str());
        lua_rawset(L, -3);

        // ArmyIndex (1-based)
        lua_pushstring(L, "ArmyIndex");
        lua_pushnumber(L, static_cast<int>(i) + 1);
        lua_rawset(L, -3);

        // Faction — default 1 (UEF)
        lua_pushstring(L, "Faction");
        lua_pushnumber(L, 1);
        lua_rawset(L, -3);

        // Team — default to FFA (each army own team)
        lua_pushstring(L, "Team");
        lua_pushnumber(L, static_cast<int>(i) + 1);
        lua_rawset(L, -3);

        // AIPersonality — empty string for human, empty for all in headless
        lua_pushstring(L, "AIPersonality");
        lua_pushstring(L, "");
        lua_rawset(L, -3);

        // StartSpot (needed by some Lua code)
        lua_pushstring(L, "StartSpot");
        lua_pushnumber(L, static_cast<int>(i) + 1);
        lua_rawset(L, -3);

        lua_rawset(L, setup_idx); // ArmySetup[army_name] = entry
    }

    lua_rawset(L, si_idx); // ScenarioInfo.ArmySetup = setup table
    lua_pop(L, 1);         // pop ScenarioInfo
}

Result<void> SessionManager::call_setup_session(lua_State* L) {
    lua_getglobal(L, "SetupSession");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return Error("SetupSession is not defined");
    }

    int status = lua_pcall(L, 0, 0, 0);
    if (status != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1)
                                               : "unknown error";
        lua_pop(L, 1);
        return Error("SetupSession() error: " + err);
    }
    return {};
}

void SessionManager::extract_start_positions(lua_State* L,
                                              sim::SimState& sim) {
    // Navigate: Scenario.MasterChain['_MASTERCHAIN_'].Markers
    lua_getglobal(L, "Scenario");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    int scenario_idx = lua_gettop(L);

    lua_pushstring(L, "MasterChain");
    lua_gettable(L, scenario_idx);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return; }
    int mc_idx = lua_gettop(L);

    lua_pushstring(L, "_MASTERCHAIN_");
    lua_gettable(L, mc_idx);
    if (!lua_istable(L, -1)) { lua_pop(L, 3); return; }
    int chain_idx = lua_gettop(L);

    lua_pushstring(L, "Markers");
    lua_gettable(L, chain_idx);
    if (!lua_istable(L, -1)) { lua_pop(L, 4); return; }
    int markers_idx = lua_gettop(L);

    // Iterate markers looking for army start positions
    lua_pushnil(L);
    while (lua_next(L, markers_idx) != 0) {
        // key at -2, value at -1
        if (lua_isstring(L, -2) && lua_istable(L, -1)) {
            const char* marker_name = lua_tostring(L, -2);
            int marker_idx = lua_gettop(L);

            // Check if this is an army marker (name starts with "ARMY_")
            std::string name_str(marker_name);
            if (name_str.find("ARMY_") == 0) {
                // Read position = {x, y, z}
                lua_pushstring(L, "position");
                lua_gettable(L, marker_idx);
                if (lua_istable(L, -1)) {
                    int pos_idx = lua_gettop(L);
                    lua_pushnumber(L, 1);
                    lua_gettable(L, pos_idx);
                    f32 x = static_cast<f32>(lua_tonumber(L, -1));
                    lua_pop(L, 1);

                    lua_pushnumber(L, 2);
                    lua_gettable(L, pos_idx);
                    f32 y = static_cast<f32>(lua_tonumber(L, -1));
                    lua_pop(L, 1);

                    lua_pushnumber(L, 3);
                    lua_gettable(L, pos_idx);
                    f32 z = static_cast<f32>(lua_tonumber(L, -1));
                    lua_pop(L, 1);

                    // Find matching army brain and set position
                    auto* brain = sim.get_army_by_name(name_str);
                    if (brain) {
                        brain->set_start_position({x, y, z});
                        spdlog::debug("  {} start pos: ({:.0f}, {:.1f}, {:.0f})",
                                       name_str, x, y, z);
                    }
                }
                lua_pop(L, 1); // pop position table
            }
        }
        lua_pop(L, 1); // pop value, keep key for next iteration
    }

    lua_pop(L, 4); // markers, chain, masterchain, Scenario
}

Result<void> SessionManager::create_army_brain(lua_State* L,
                                                sim::SimState& sim,
                                                i32 index,
                                                const std::string& name,
                                                const std::string& nickname) {
    // Get the C++ ArmyBrain (already created in main.cpp when armies added)
    auto* brain = sim.get_army(index);
    if (!brain) {
        return Error("Army index out of range: " + std::to_string(index));
    }

    // Create a Lua table for this brain
    lua_newtable(L);
    int brain_tbl = lua_gettop(L);

    // Store _c_object = lightuserdata(brain)
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, brain);
    lua_rawset(L, brain_tbl);

    // Set metatable to moho.aibrain_methods (after Flatten by globalInit)
    lua_getglobal(L, "moho");
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "aibrain_methods");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            lua_setmetatable(L, brain_tbl);
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1); // pop moho

    // Store Name field in the Lua table
    lua_pushstring(L, "Name");
    lua_pushstring(L, name.c_str());
    lua_rawset(L, brain_tbl);

    // Store Nickname
    lua_pushstring(L, "Nickname");
    lua_pushstring(L, nickname.c_str());
    lua_rawset(L, brain_tbl);

    // OnBeginSession — no-op method (Lua code calls brain:OnBeginSession())
    lua_pushstring(L, "OnBeginSession");
    lua_pushcfunction(L, [](lua_State*) -> int { return 0; });
    lua_rawset(L, brain_tbl);

    // Store a reference in the Lua registry so we can retrieve it later
    lua_pushvalue(L, brain_tbl);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    brain->set_lua_table_ref(ref);

    // Call OnCreateArmyBrain(index, brain, name, nickname)
    lua_getglobal(L, "OnCreateArmyBrain");
    if (lua_isfunction(L, -1)) {
        lua_pushnumber(L, index + 1);  // 1-based index for Lua
        lua_pushvalue(L, brain_tbl);    // brain table
        lua_pushstring(L, name.c_str());
        lua_pushstring(L, nickname.c_str());

        int status = lua_pcall(L, 4, 0, 0);
        if (status != 0) {
            const char* err = lua_tostring(L, -1);
            spdlog::warn("  OnCreateArmyBrain({}) error: {}", name,
                          err ? err : "unknown");
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1); // pop non-function
    }

    lua_pop(L, 1); // pop brain table
    return {};
}

Result<void> SessionManager::call_begin_session(lua_State* L) {
    lua_getglobal(L, "BeginSession");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return Error("BeginSession is not defined");
    }

    int status = lua_pcall(L, 0, 0, 0);
    if (status != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1)
                                               : "unknown error";
        lua_pop(L, 1);
        return Error("BeginSession() error: " + err);
    }
    return {};
}

} // namespace osc::lua
