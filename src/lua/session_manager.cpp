#include "lua/session_manager.hpp"
#include "lua/lua_state.hpp"
#include "sim/army_brain.hpp"
#include "sim/platoon.hpp"
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

    // If any armies are AI, set ScenarioInfo.GameHasAIs = true so
    // BeginSession loads AI templates (SetupSession resets it to false)
    if (!ai_army_indices_.empty()) {
        lua_getglobal(L, "ScenarioInfo");
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "GameHasAIs");
            lua_pushboolean(L, 1);
            lua_rawset(L, -3);
        }
        lua_pop(L, 1);
    }

    // Step 3: Extract start positions from Scenario.MasterChain markers
    extract_start_positions(L, sim);

    // Step 4: Create army brains
    // Determine how many armies to create (max_armies_ limits non-civilian count)
    size_t army_limit = meta.armies.size();
    if (max_armies_ > 0) {
        army_limit = std::min(meta.armies.size(), static_cast<size_t>(max_armies_));
    }
    spdlog::info("  Creating army brains ({} of {} armies)...",
                 army_limit, meta.armies.size());
    i32 brains_created = 0;
    for (size_t i = 0; i < army_limit; i++) {
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

    // Step 6: Ensure each non-civilian army has at least one unit (ACU).
    //         FA's OnPopulate may fail in our engine, so we create ACUs
    //         directly via the sim C++ API if they weren't spawned.
    for (size_t i = 0; i < army_limit; i++) {
        auto* brain = sim.get_army(static_cast<i32>(i));
        if (!brain || brain->is_civilian()) continue;
        if (brain->get_unit_cost_total(sim.entity_registry()) > 0) continue;

        // Determine ACU blueprint from faction
        const char* acu_bp = "uel0001"; // UEF default
        int faction = brain->faction();
        if (faction == 2) acu_bp = "ual0001"; // Aeon
        else if (faction == 3) acu_bp = "url0001"; // Cybran
        else if (faction == 4) acu_bp = "xsl0001"; // Seraphim

        const auto& pos = brain->start_position();
        spdlog::info("  Army {} has no units — spawning ACU {} at ({:.0f}, {:.0f})",
                     meta.armies[i], acu_bp, pos.x, pos.z);

        // Call CreateUnit(bp, army_1based, x, y, z) via Lua stack
        lua_getglobal(L, "CreateUnit");
        if (lua_isfunction(L, -1)) {
            lua_pushstring(L, acu_bp);
            lua_pushnumber(L, static_cast<int>(i) + 1); // 1-based
            lua_pushnumber(L, pos.x);
            lua_pushnumber(L, pos.y);
            lua_pushnumber(L, pos.z);
            int status = lua_pcall(L, 5, 1, 0);
            if (status != 0) {
                spdlog::warn("  CreateUnit ACU failed: {}",
                             lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown");
                lua_pop(L, 1);
            } else {
                // Give initial resources (650 mass storage, 4000 energy storage)
                auto& econ = brain->economy();
                econ.mass.stored = std::min(econ.mass.stored + 650.0,
                                            econ.mass.max_storage);
                econ.energy.stored = std::min(econ.energy.stored + 4000.0,
                                              econ.energy.max_storage);
                lua_pop(L, 1); // pop unit table
            }
        } else {
            lua_pop(L, 1);
        }
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

    // Cheat multipliers (used by SetupCheat when personality ends with "cheat")
    set_opt_num("CheatMult", cheat_mult_);
    set_opt_num("BuildMult", build_mult_);
    set_opt_str("OmniCheat", "off");

    // RestrictedCategories = {} (empty table)
    lua_pushstring(L, "RestrictedCategories");
    lua_newtable(L);
    lua_rawset(L, opts_idx);

    lua_rawset(L, si_idx); // ScenarioInfo.Options = opts table

    // Create ScenarioInfo.ArmySetup = {}
    lua_pushstring(L, "ArmySetup");
    lua_newtable(L);
    int setup_idx = lua_gettop(L);

    size_t setup_limit = meta.armies.size();
    if (max_armies_ > 0) {
        setup_limit = std::min(meta.armies.size(), static_cast<size_t>(max_armies_));
    }
    for (size_t i = 0; i < setup_limit; i++) {
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

        // AIPersonality — configurable for AI armies, empty for human/civilian
        lua_pushstring(L, "AIPersonality");
        lua_pushstring(L, is_ai ? ai_personality_.c_str() : "");
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

    // Store sim generation for stale-handle detection across reloads
    lua_pushstring(L, "_c_sim_gen");
    lua_pushnumber(L, static_cast<f64>(sim::SimState::sim_generation()));
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

    // Create the ArmyPool platoon — the engine auto-creates this for every brain.
    // FA code assumes it always exists (e.g. GetPlatoonUniquelyNamed('ArmyPool')).
    {
        auto* pool = brain->create_platoon("ArmyPool");
        lua_newtable(L);
        int ptbl = lua_gettop(L);

        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, pool);
        lua_rawset(L, ptbl);

        lua_pushstring(L, "PlanName");
        lua_pushstring(L, "");
        lua_rawset(L, ptbl);

        // Set metatable: try __platoon_class (FA's Platoon class), else raw moho
        lua_pushstring(L, "__platoon_class");
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_pushstring(L, "__osc_platoon_mt");
            lua_rawget(L, LUA_REGISTRYINDEX);
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                // Build from moho.platoon_methods
                lua_newtable(L);
                lua_pushstring(L, "__index");
                lua_getglobal(L, "moho");
                if (lua_istable(L, -1)) {
                    lua_pushstring(L, "platoon_methods");
                    lua_rawget(L, -2);
                    lua_remove(L, -2); // remove moho
                } else {
                    lua_pop(L, 1);
                    lua_newtable(L); // empty fallback
                }
                lua_rawset(L, -3);
                // Cache it
                lua_pushstring(L, "__osc_platoon_mt");
                lua_pushvalue(L, -2);
                lua_rawset(L, LUA_REGISTRYINDEX);
            }
        }
        lua_setmetatable(L, ptbl);

        lua_pushvalue(L, ptbl);
        int pool_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        pool->set_lua_table_ref(pool_ref);

        // Call OnCreate(self, plan) to initialize Trash, PlatoonData, etc.
        lua_pushstring(L, "OnCreate");
        lua_gettable(L, ptbl);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, ptbl);  // self
            lua_pushstring(L, "");   // plan (empty for ArmyPool)
            if (lua_pcall(L, 2, 0, 0) != 0) {
                spdlog::warn("ArmyPool OnCreate error: {}",
                              lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }

        lua_pop(L, 1); // pop platoon table
    }

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
