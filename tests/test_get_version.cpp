// GetVersion answers Moho's engine version in every state (GetEngineVersion:
// 1.6 and 6 in the Steam exe), which retail's main menu shows, and takes no
// argument.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/engine_bindings.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/sim_state.hpp"

#include <string>

extern "C" {
#include <lua.h>
}

namespace {

std::string global_string(osc::lua::LuaState& state, const char* name) {
    lua_getglobal(state.raw(), name);
    const char* s = lua_tostring(state.raw(), -1);
    std::string out = s ? s : "";
    lua_pop(state.raw(), 1);
    return out;
}

} // namespace

TEST_CASE("GetVersion answers Moho's engine version in the UI and sim states", "[lua][version]") {
    osc::lua::LuaState ui;
    osc::lua::register_blueprint_bindings(ui);
    REQUIRE(ui.do_string("version = GetVersion()").ok());
    CHECK(global_string(ui, "version") == "1.6.6");

    osc::lua::LuaState sim_lua;
    osc::blueprints::BlueprintStore store{sim_lua.raw()};
    osc::sim::SimState sim{sim_lua.raw(), &store};
    osc::lua::register_blueprint_bindings(sim_lua);
    osc::lua::register_moho_bindings(sim_lua, sim);
    osc::lua::register_sim_bindings(sim_lua, sim);
    REQUIRE(sim_lua.do_string("version = GetVersion()").ok());
    CHECK(global_string(sim_lua, "version") == "1.6.6");
}

TEST_CASE("GetVersion takes no argument, as Moho's", "[lua][version]") {
    osc::lua::LuaState ui;
    osc::lua::register_blueprint_bindings(ui);
    REQUIRE(ui.do_string("ok, message = pcall(GetVersion, 1)").ok());
    lua_getglobal(ui.raw(), "ok");
    CHECK(lua_toboolean(ui.raw(), -1) == 0);
    lua_pop(ui.raw(), 1);
    CHECK(
        global_string(ui, "message").find("GetVersion() -> string\n  expected 0 args, but got 1") !=
        std::string::npos);
}
