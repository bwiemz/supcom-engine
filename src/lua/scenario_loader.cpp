#include "lua/scenario_loader.hpp"
#include "lua/lua_state.hpp"
#include "map/heightmap.hpp"
#include "map/scmap_parser.hpp"
#include "map/terrain.hpp"
#include "sim/sim_state.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::lua {

namespace {

/// Read a string field from the table at the given stack index.
/// Returns empty string if field doesn't exist or isn't a string.
std::string read_string_field(lua_State* L, int table_idx, const char* key) {
    lua_pushstring(L, key);
    lua_gettable(L, table_idx);
    std::string result;
    if (lua_isstring(L, -1)) {
        result = lua_tostring(L, -1);
    }
    lua_pop(L, 1);
    return result;
}

/// Read a numeric field from the table at the given stack index.
f64 read_number_field(lua_State* L, int table_idx, const char* key,
                      f64 default_val = 0.0) {
    lua_pushstring(L, key);
    lua_gettable(L, table_idx);
    f64 result = default_val;
    if (lua_isnumber(L, -1)) {
        result = lua_tonumber(L, -1);
    }
    lua_pop(L, 1);
    return result;
}

/// Extract army names from ScenarioInfo.Configurations.standard.teams[1].armies
std::vector<std::string> extract_armies(lua_State* L, int scenario_idx) {
    std::vector<std::string> armies;

    // ScenarioInfo.Configurations
    lua_pushstring(L, "Configurations");
    lua_gettable(L, scenario_idx);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return armies; }
    int configs_idx = lua_gettop(L);

    // .standard
    lua_pushstring(L, "standard");
    lua_gettable(L, configs_idx);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return armies; }
    int standard_idx = lua_gettop(L);

    // .teams
    lua_pushstring(L, "teams");
    lua_gettable(L, standard_idx);
    if (!lua_istable(L, -1)) { lua_pop(L, 3); return armies; }
    int teams_idx = lua_gettop(L);

    // teams[1]
    lua_pushnumber(L, 1);
    lua_gettable(L, teams_idx);
    if (!lua_istable(L, -1)) { lua_pop(L, 4); return armies; }
    int team1_idx = lua_gettop(L);

    // .armies
    lua_pushstring(L, "armies");
    lua_gettable(L, team1_idx);
    if (!lua_istable(L, -1)) { lua_pop(L, 5); return armies; }
    int armies_idx = lua_gettop(L);

    // Iterate array
    for (int i = 1; ; i++) {
        lua_pushnumber(L, i);
        lua_gettable(L, armies_idx);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            break;
        }
        if (lua_isstring(L, -1)) {
            armies.emplace_back(lua_tostring(L, -1));
        }
        lua_pop(L, 1);
    }

    lua_pop(L, 5); // configs, standard, teams, team1, armies
    return armies;
}

} // namespace

Result<ScenarioMetadata> ScenarioLoader::load_scenario(
    LuaState& state,
    const vfs::VirtualFileSystem& vfs,
    const std::string& scenario_path,
    sim::SimState& sim) {

    spdlog::info("Loading scenario: {}", scenario_path);

    lua_State* L = state.raw();

    // Step 0: Register scenario format helper functions.
    // Scenario/save files use STRING(), VECTOR3(), GROUP() etc. constructors.
    // Must use rawset to bypass config.lua strict mode metamethods.
    auto rawset_cfunc = [L](const char* name, lua_CFunction fn) {
        lua_pushstring(L, name);
        lua_pushcclosure(L, fn, 0);
        lua_rawset(L, LUA_GLOBALSINDEX);
    };

    // STRING(s) — identity function
    rawset_cfunc("STRING", [](lua_State* Ls) -> int {
        lua_pushvalue(Ls, 1);
        return 1;
    });

    // VECTOR3(x, y, z) — creates {x, y, z} table
    rawset_cfunc("VECTOR3", [](lua_State* Ls) -> int {
        f64 x = luaL_checknumber(Ls, 1);
        f64 y = luaL_checknumber(Ls, 2);
        f64 z = luaL_checknumber(Ls, 3);
        lua_newtable(Ls);
        lua_pushnumber(Ls, 1); lua_pushnumber(Ls, x); lua_settable(Ls, -3);
        lua_pushnumber(Ls, 2); lua_pushnumber(Ls, y); lua_settable(Ls, -3);
        lua_pushnumber(Ls, 3); lua_pushnumber(Ls, z); lua_settable(Ls, -3);
        return 1;
    });

    // GROUP(t), BOOLEAN(b), FLOAT(f), RECTANGLE(t) — identity functions
    auto identity = [](lua_State* Ls) -> int {
        lua_pushvalue(Ls, 1);
        return 1;
    };
    rawset_cfunc("GROUP", identity);
    rawset_cfunc("BOOLEAN", identity);
    rawset_cfunc("FLOAT", identity);
    rawset_cfunc("RECTANGLE", identity);

    // Step 1: Load and execute the _scenario.lua file.
    // It sets the global ScenarioInfo table.
    auto scenario_data = vfs.read_file(scenario_path);
    if (!scenario_data) {
        return Error("Scenario file not found in VFS: " + scenario_path);
    }

    auto exec_result = state.do_buffer(
        scenario_data->data(), scenario_data->size(),
        ("@" + scenario_path).c_str());
    if (!exec_result) {
        return Error("Failed to execute scenario: " + exec_result.error().message);
    }

    // Step 2: Read ScenarioInfo from the Lua global
    lua_getglobal(L, "ScenarioInfo");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return Error("ScenarioInfo global not set by scenario file");
    }
    int si_idx = lua_gettop(L);

    ScenarioMetadata meta;
    meta.name = read_string_field(L, si_idx, "name");
    meta.type = read_string_field(L, si_idx, "type");
    meta.scmap_path = read_string_field(L, si_idx, "map");
    meta.save_path = read_string_field(L, si_idx, "save");
    meta.script_path = read_string_field(L, si_idx, "script");

    // size = {width, height}
    lua_pushstring(L, "size");
    lua_gettable(L, si_idx);
    if (lua_istable(L, -1)) {
        int size_idx = lua_gettop(L);
        lua_pushnumber(L, 1);
        lua_gettable(L, size_idx);
        meta.map_width = static_cast<u32>(lua_tonumber(L, -1));
        lua_pop(L, 1);

        lua_pushnumber(L, 2);
        lua_gettable(L, size_idx);
        meta.map_height = static_cast<u32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // size table

    // Extract army names
    meta.armies = extract_armies(L, si_idx);

    lua_pop(L, 1); // ScenarioInfo

    spdlog::info("  Map: {} ({}x{}), type: {}", meta.name,
                 meta.map_width, meta.map_height, meta.type);
    if (!meta.armies.empty()) {
        spdlog::info("  Armies: {}", meta.armies.size());
    }

    // Step 3: Load the .scmap file
    if (meta.scmap_path.empty()) {
        return Error("ScenarioInfo.map is empty");
    }

    auto scmap_data = vfs.read_file(meta.scmap_path);
    if (!scmap_data) {
        return Error("SCMAP file not found in VFS: " + meta.scmap_path);
    }

    // Convert string data to u8 vector for parser
    std::vector<u8> scmap_bytes(scmap_data->begin(), scmap_data->end());
    auto parse_result = map::parse_scmap(scmap_bytes);
    if (!parse_result) {
        return Error("Failed to parse SCMAP: " + parse_result.error().message);
    }

    auto& scmap = parse_result.value();
    spdlog::info("  Heightmap: {}x{}, scale: {}, water: {}",
                 scmap.map_width + 1, scmap.map_height + 1,
                 scmap.height_scale,
                 scmap.has_water ? scmap.water_elevation : 0.0f);

    // Step 4: Build Terrain and install into SimState
    map::Heightmap heightmap(scmap.map_width, scmap.map_height,
                              scmap.height_scale,
                              std::move(scmap.heightmap));

    f32 water_elev = scmap.has_water ? scmap.water_elevation : 0.0f;
    auto terrain = std::make_unique<map::Terrain>(
        std::move(heightmap), water_elev, scmap.has_water);

    sim.set_terrain(std::move(terrain));
    sim.build_pathfinding_grid();
    sim.build_visibility_grid();

    return meta;
}

} // namespace osc::lua
