#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/entity.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lua.h>
}

#include <cmath>
#include <string>

using Catch::Approx;
using osc::sim::Quaternion;
using osc::sim::Scroll;
using osc::sim::ScrollerSpec;
using osc::sim::ScrollType;
using osc::sim::TextureScroller;
using osc::sim::Vector3;

namespace {

struct Beat {
    Scroll start, end;
};

TextureScroller made(const ScrollerSpec& spec, Beat& beat, Vector3 at = {}, Quaternion q = {}) {
    TextureScroller s;
    s.last_position = at;
    s.last_orientation = q;
    s.set(spec, beat.start, beat.end);
    return s;
}

} // namespace

TEST_CASE("A thread scroller scrolls each side by how far it moved forward", "[scroller]") {
    ScrollerSpec spec;
    spec.type = ScrollType::MotionDerived;
    spec.side_dist = 1.0f;
    spec.scroll_mult = 0.2f;
    Beat beat;
    TextureScroller s = made(spec, beat);

    s.tick({0, 0, 0.5f}, {}, beat.start, beat.end);
    CHECK(beat.start.u == 0.0f);
    CHECK(beat.end.u == Approx(0.1f));
    CHECK(beat.end.v == Approx(0.1f));

    s.tick({0, 0, 0.5f}, {0, std::sin(0.3f), 0, std::cos(0.3f)}, beat.start, beat.end);
    CHECK(beat.start.u == 0.0f);
    CHECK(beat.end.u == Approx(0.1f));

    const float h = std::sqrt(0.5f);
    s.last_orientation = {};
    s.tick({0, 0, 1.5f}, {0, h, 0, h}, beat.start, beat.end);
    CHECK(beat.start.u == Approx(0.1f));
    CHECK(beat.end.u == Approx(0.0f).margin(1e-6));
    CHECK(beat.end.v == Approx(0.4f));
}

TEST_CASE("A ping-pong scroller holds each offset for its time, per lane", "[scroller]") {
    ScrollerSpec spec;
    spec.type = ScrollType::PingPong;
    spec.ping = {0.006f, 0.5f};
    spec.pong = {0.039f, 0.25f};
    spec.ping_seconds[0] = 2.5f;
    spec.pong_seconds[0] = 2.5f;
    spec.ping_seconds[1] = 1.0f;
    spec.pong_seconds[1] = 0.5f;
    Beat beat;
    TextureScroller s = made(spec, beat);

    auto run = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
            s.tick({}, {}, beat.start, beat.end);
        }
    };
    run(1);
    CHECK(beat.start.u == 0.006f);
    CHECK(beat.end.u == 0.006f);
    CHECK(beat.end.v == 0.5f);
    run(10);
    CHECK(beat.end.u == 0.006f);
    CHECK(beat.end.v == 0.25f);
    run(5);
    CHECK(beat.end.v == 0.5f);
    run(9);
    CHECK(beat.end.u == 0.006f);
    run(1);
    CHECK(beat.end.u == 0.039f);
    CHECK(beat.start.u == 0.039f);
}

TEST_CASE("A manual scroller drifts each tick; removing it stops the drift", "[scroller]") {
    ScrollerSpec spec;
    spec.type = ScrollType::Manual;
    spec.rate = {0.25f, -0.5f};
    Beat beat;
    TextureScroller s = made(spec, beat);
    for (int i = 0; i < 3; ++i) {
        s.tick({}, {}, beat.start, beat.end);
    }
    CHECK(beat.start.u == 0.5f);
    CHECK(beat.end.u == 0.75f);
    CHECK(beat.end.v == -1.5f);

    s.set({}, beat.start, beat.end);
    CHECK(beat.end.u == 0.5f);
    CHECK(beat.end.v == -1.0f);
    s.tick({0, 0, 1}, {}, beat.start, beat.end);
    CHECK(beat.end.u == 0.5f);
}

TEST_CASE("The scroller bindings drive an entity's scroll", "[scroller]") {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};
    osc::lua::register_moho_bindings(state, sim);
    osc::lua::register_sim_bindings(state, sim);
    sim.add_army("ARMY_1", "ARMY_1");
    lua_State* L = state.raw();
    REQUIRE(
        state.do_string("return {BlueprintId = 'test_tank', Defense = {MaxHealth = 100}}").ok());
    store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
    lua_pop(L, 1);
    store.expose_to_lua(L);
    REQUIRE(state
                .do_string("TestTank = setmetatable({}, {__index = moho.entity_methods})\n"
                           "TestTank.__index = TestTank")
                .ok());
    lua_pushstring(L, "__osc_unit_script_classes");
    lua_newtable(L);
    lua_pushstring(L, "test_tank");
    lua_getglobal(L, "TestTank");
    lua_rawset(L, -3);
    lua_rawset(L, LUA_REGISTRYINDEX);
    REQUIRE(state.do_string("tank = CreateUnit('test_tank', 1, 0, 0, 0)").ok());
    REQUIRE(state.do_string("return tank.EntityId").ok());
    const auto id = static_cast<osc::u32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    osc::sim::Entity* tank = sim.entity_registry().find(id);
    REQUIRE(tank);
    auto run = [&](const char* code) {
        auto r = state.do_string(code);
        return r.ok() ? std::string() : r.error().message;
    };

    REQUIRE(run("tank:AddManualScroller(0.25, 0.5)").empty());
    sim.tick();
    sim.tick();
    CHECK(tank->scroll_start().u == 0.25f);
    CHECK(tank->scroll_end().u == 0.5f);
    CHECK(tank->scroll_end().v == 1.0f);

    REQUIRE(run("tank:RemoveScroller()").empty());
    CHECK(tank->scroll_end().u == 0.25f);
    CHECK(tank->scroll_end().v == 0.5f);
    REQUIRE(run("tank:AddThreadScroller(1.0, 0.2)").empty());
    sim.tick();
    tank->set_position({tank->position().x, tank->position().y, tank->position().z + 0.5f});
    sim.tick();
    CHECK(tank->scroll_end().u == Approx(0.25f + 0.1f));
    CHECK(tank->scroll_end().v == Approx(0.5f + 0.1f));

    CHECK(run("tank:AddPingPongScroller(0.1, 0, 0, 0, 0, 0)").find("expected 9 args, but got 7") !=
          std::string::npos);
}
