#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "sim/entity.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <string>

TEST_CASE("SetHealth and AdjustHealth keep an entity's health between 0 and its max", "[lua]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::lua::register_moho_bindings(lua, sim);
    auto owned = std::make_unique<osc::sim::Entity>();
    osc::sim::Entity& entity = *owned;
    sim.entity_registry().register_entity(std::move(owned));
    entity.set_max_health(100);
    lua_State* L = lua.raw();
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, &entity);
    lua_rawset(L, -3);
    lua_setglobal(L, "e");
    const auto health_after = [&](const std::string& call) {
        REQUIRE(lua.do_string("moho.entity_methods." + call).ok());
        return entity.health();
    };

    CHECK(health_after("SetHealth(e, nil, 40)") == 40.0f);
    CHECK(health_after("SetHealth(e, nil, 250)") == 100.0f);
    CHECK(health_after("SetHealth(e, nil, -30)") == 0.0f);
    CHECK(health_after("AdjustHealth(e, nil, 60)") == 60.0f);
    CHECK(health_after("AdjustHealth(e, nil, 70)") == 100.0f);
    CHECK(health_after("AdjustHealth(e, nil, -150)") == 0.0f);
}
