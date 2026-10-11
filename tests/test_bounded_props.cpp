#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "sim/bounded_props.hpp"
#include "sim/prop.hpp"
#include "sim/sim_state.hpp"
#include "sim/state_io.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <memory>
#include <string>
#include <vector>

using osc::sim::BoundedProps;
using osc::sim::Prop;
using osc::sim::SimState;

namespace {

struct Wrecks {
    osc::lua::LuaState lua;
    SimState sim{lua.raw(), nullptr};
    std::vector<osc::u32> ids;

    Wrecks() { osc::lua::register_moho_bindings(lua, sim); }

    osc::u32 add(double mass) {
        auto p = std::make_unique<Prop>();
        Prop* raw = p.get();
        const osc::u32 id = sim.entity_registry().register_entity(std::move(p));
        lua_State* L = lua.raw();
        lua_newtable(L);
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, raw);
        lua_rawset(L, -3);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "wreck");
        raw->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
        REQUIRE(
            lua.do_string("moho.prop_methods.AddBoundedProp(wreck, " + std::to_string(mass) + ")")
                .ok());
        ids.push_back(id);
        return id;
    }

    bool alive(osc::u32 id) const { return sim.entity_registry().find(id) != nullptr; }
};

} // namespace

TEST_CASE("Bounded props leave lowest priority first, then oldest", "[sim][props]") {
    BoundedProps bounded;
    std::vector<Prop> props(4);
    bounded.insert(50, 3, &props[0]);
    bounded.insert(20, 7, &props[1]);
    const osc::i32 old20 = bounded.insert(20, 2, &props[2]);
    bounded.insert(90, 1, &props[3]);

    CHECK(bounded.lowest() == &props[2]);
    bounded.remove(old20);
    CHECK(bounded.lowest() == &props[1]);
    bounded.pop_lowest();
    CHECK(bounded.lowest() == &props[0]);
    bounded.pop_lowest();
    CHECK(bounded.lowest() == &props[3]);
    CHECK(bounded.size() == 1);
}

TEST_CASE("AddBoundedProp destroys the cheapest wreck past 1000", "[sim][props][lua]") {
    Wrecks w;
    const osc::u32 cheap = w.add(10.4);
    for (int i = 0; i < 998; ++i) {
        w.add(500.0);
    }
    const osc::u32 twelve = w.add(12.0);
    REQUIRE(w.sim.bounded_props().size() == 1000);
    CHECK(w.alive(cheap));

    w.add(300.0);
    CHECK_FALSE(w.alive(cheap));
    CHECK(w.alive(twelve));
    CHECK(w.sim.bounded_props().size() == 1000);
}

TEST_CASE("A destroyed bounded prop leaves the count when freed", "[sim][props][lua]") {
    Wrecks w;
    w.add(5.0);
    REQUIRE(w.lua.do_string("first = wreck").ok());
    const osc::u32 second = w.add(6.0);
    REQUIRE(w.lua.do_string("moho.prop_methods.Destroy(first)").ok());
    CHECK(w.sim.bounded_props().size() == 2);
    w.sim.tick();
    CHECK(w.sim.bounded_props().size() == 1);
    CHECK(w.sim.bounded_props().lowest() == w.sim.entity_registry().find(second));
}

TEST_CASE("Bounded props survive a save and load", "[sim][props][save]") {
    Wrecks w;
    w.add(40.0);
    const osc::u32 cheap = w.add(7.0);
    const auto bytes = osc::sim::save_sim_state(w.sim);

    osc::lua::LuaState lua;
    SimState loaded(lua.raw(), nullptr);
    REQUIRE(osc::sim::load_sim_state(loaded, bytes).empty());
    CHECK(loaded.bounded_props().size() == 2);
    CHECK(loaded.bounded_props().lowest() == loaded.entity_registry().find(cheap));
}
