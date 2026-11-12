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
#include <vector>

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

TEST_CASE("A whacked tree turns as the original game's, to the bit", "[prop]") {
    struct Turn {
        int tick;
        Quaternion q;
    };
    struct Case {
        const char* name;
        f32 nx, nz, force;
        bool breaks;
        std::vector<Turn> turns;
    };
    // Retail's, probed (tree-fall): trees made at heading 0, whacked, their orientation each tick.
    const std::vector<Case> cases{
        {"b1",
         1,
         0,
         1,
         true,
         {{1, {-2.09563567e-008f, 0, -0.47942552f, 0.87758255f}},
          {2, {-3.09086197e-008f, -2.66453526e-015f, -0.707106769f, 0.707106829f}}}},
        {"b2",
         0.6f,
         0.8f,
         1,
         true,
         {{1, {0.383540422f, 0, -0.287655324f, 0.87758255f}},
          {2, {0.565685451f, 0, -0.424264133f, 0.707106829f}}}},
        {"b3",
         -0.3f,
         -0.954f,
         1,
         true,
         {{1, {-0.457345426f, 0, 0.143819377f, 0.87758255f}},
          {2, {-0.674540699f, 1.4901163e-008f, 0.212119818f, 0.707106769f}}}},
        {"w1",
         0,
         1,
         0.25f,
         true,
         {{1, {0.124674723f, 0, 0, 0.992197692f}},
          {2, {0.259495735f, 0, 0, 0.965744317f}},
          {3, {0.413459301f, 0, 0, 0.910522699f}},
          {4, {0.591263711f, 0, 0, 0.806478322f}},
          {5, {0.707106829f, 0, 0, 0.707106829f}}}},
        {"s1",
         0.8f,
         -0.6f,
         0.3f,
         false,
         {{1, {-0.0896629095f, 0, -0.119550526f, 0.9887712f}},
          {3, {-0.0449577682f, 1.86264493e-009f, -0.0599437021f, 0.997188807f}},
          {4, {7.33892769e-009f, 1.62981473e-009f, 1.4901163e-008f, 1}},
          {5, {0.0224947315f, 1.74375037e-009f, 0.0299929753f, 0.999297082f}},
          {7, {0.0112493271f, 1.86264493e-009f, 0.0149991093f, 0.999824226f}},
          {8, {-1.30105784e-008f, 1.84809323e-009f, -1.86264515e-008f, 1}}}},
        {"s2",
         -1,
         0,
         0.05f,
         false,
         {{1, {-1.09266884e-009f, 0, 0.0249973498f, 0.999687493f}},
          {4, {7.77156117e-016f, 0, -1.86264515e-008f, 1}}}},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        Prop tree;
        tree.fall_down(1.0f);
        tree.whack(c.nx, c.nz, c.force, c.breaks);
        Quaternion want{0, 0, 0, 1};
        size_t next = 0;
        for (int t = 1; t <= 15; ++t) {
            tree.step_fall(nullptr);
            if (next < c.turns.size() && c.turns[next].tick == t) {
                want = c.turns[next++].q;
            }
            INFO("tick " << t);
            const Quaternion& q = tree.orientation();
            CHECK(q.x == want.x);
            CHECK(q.y == want.y);
            CHECK(q.z == want.z);
            CHECK(q.w == want.w);
        }
    }
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
