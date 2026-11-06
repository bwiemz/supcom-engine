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

using namespace osc;

namespace {

sim::Entity& add_entity(lua::LuaState& lua, sim::SimState& sim, const char* name,
                        const sim::Vector3& at) {
    auto owned = std::make_unique<sim::Entity>();
    sim::Entity& entity = *owned;
    sim.entity_registry().register_entity(std::move(owned));
    entity.set_position(at);
    entity.place_now();
    lua_State* L = lua.raw();
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, &entity);
    lua_rawset(L, -3);
    lua_setglobal(L, name);
    return entity;
}

} // namespace

TEST_CASE("GetPosition refills one table per entity, as Moho's position cache", "[lua]") {
    lua::LuaState lua;
    sim::SimState sim(lua.raw(), nullptr);
    lua::register_moho_bindings(lua, sim);
    sim::Entity& a = add_entity(lua, sim, "a", {10, 5, 20});
    add_entity(lua, sim, "b", {30, 6, 40});
    const auto check = [&](const std::string& code) {
        const auto r = lua.do_string(code);
        INFO(code);
        CHECK(r.ok());
    };

    check("p = moho.entity_methods.GetPosition(a) p[2] = p[2] - 2 edited = p[2] "
          "q = moho.entity_methods.GetPosition(a)");
    check("assert(rawequal(p, q))");
    check("assert(edited == 3 and p[2] == 5 and p.y == 5)");
    check("other = moho.entity_methods.GetPosition(b) "
          "assert(not rawequal(p, other) and other[1] == 30 and p[1] == 10)");

    a.set_position({11, 5, 21});
    a.place_now();
    check(
        "assert(p[1] == 10) moho.entity_methods.GetPosition(a) assert(p[1] == 11 and p[3] == 21)");
}
