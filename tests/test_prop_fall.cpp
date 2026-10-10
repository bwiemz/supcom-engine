#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/prop.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>

using namespace osc;
using namespace osc::sim;
using Catch::Matchers::WithinAbs;

namespace {

Vector3 up_of(const Prop& p) {
    return quat_rotate(p.orientation(), {0, 1, 0});
}

} // namespace

TEST_CASE("A whacked tree falls over tick by tick toward the whack", "[prop]") {
    Prop tree;
    tree.fall_down(1.0f);
    tree.whack(1, 0, 0.25f, true);

    tree.step_fall(nullptr);
    CHECK_THAT(tree.fall_angle, WithinAbs(0.25, 1e-6));
    CHECK_THAT(up_of(tree).x, WithinAbs(std::sin(0.25), 1e-5));
    CHECK_THAT(up_of(tree).y, WithinAbs(std::cos(0.25), 1e-5));

    tree.step_fall(nullptr);
    CHECK_THAT(tree.fall_angle, WithinAbs(0.525, 1e-6));

    int ticks = 2;
    while (tree.fall_angle < 1.5707963f && ticks < 50) {
        tree.step_fall(nullptr);
        ++ticks;
    }
    CHECK(ticks == 5);
    CHECK_THAT(up_of(tree).x, WithinAbs(1.0, 1e-5));
    CHECK_THAT(up_of(tree).y, WithinAbs(0.0, 1e-5));
    CHECK(tree.fall_speed == 0.0f);
}

TEST_CASE("A tree whacked without breaking sways back upright", "[prop]") {
    Prop tree;
    tree.fall_down(1.0f);
    tree.whack(0, -1, 0.05f, false);
    f32 most = 0;
    for (int i = 0; i < 40; ++i) {
        tree.step_fall(nullptr);
        most = std::max(most, std::fabs(up_of(tree).z));
    }
    CHECK(most > 0.04f);
    CHECK_THAT(up_of(tree).y, WithinAbs(1.0, 1e-3));
}

TEST_CASE("MotorFallDown:Whack leaves the tree standing until its motor runs", "[prop][lua]") {
    lua::LuaState lua;
    SimState sim(lua.raw(), nullptr);
    lua::register_sim_bindings(lua, sim);
    lua::register_moho_bindings(lua, sim);
    auto owned = std::make_unique<Prop>();
    Prop& tree = *owned;
    sim.entity_registry().register_entity(std::move(owned));
    lua_State* L = lua.raw();
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, &tree);
    lua_rawset(L, -3);
    lua_setglobal(L, "tree");

    REQUIRE(lua.do_string("motor = moho.prop_methods.FallDown(tree) "
                          "whacked = motor:Whack(1, 0, 0, 0.25, true)")
                .ok());
    CHECK_THAT(up_of(tree).y, WithinAbs(1.0, 1e-6));
    CHECK(lua.do_string("assert(whacked == motor)").ok());
}
