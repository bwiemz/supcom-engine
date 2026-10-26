// The sim's debug drawing, as Moho's (faf-re Sim.cpp: cfunc_DrawLineL, cfunc_DrawLinePopL,
// cfunc_DrawCircleL into CDebugCanvas): lines and octagons the sim's scripts draw into a canvas
// each tick, which AdvanceBeat rolls over for the UI to draw for a beat. FAF's NavDebug and AI
// debugging draw with them.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/debug_canvas.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/world_snapshot.hpp"

#include <cmath>
#include <string>

using Catch::Approx;
using osc::f32;
using osc::sim::DebugCanvas;
using osc::sim::DebugLine;

TEST_CASE("DrawCircle's ring is a level octagon from its +x side, as AddWireCircle's",
          "[debugdraw]") {
    DebugCanvas canvas;
    canvas.circle({10, 5, 10}, 2, 0xff00ff00u);
    const auto& lines = canvas.lines();
    REQUIRE(lines.size() == 8);
    CHECK(lines[0].a.x == Approx(12.0f));
    CHECK(lines[0].a.z == Approx(10.0f));
    // A turn of an eighth each, toward -z first (+Z turned onto +Y: axes x and -z)
    CHECK(lines[0].b.x == Approx(10.0f + 2.0f * std::cos(0.785398f)));
    CHECK(lines[0].b.z == Approx(10.0f - 2.0f * std::sin(0.785398f)));
    CHECK(lines[1].b.x == Approx(10.0f).margin(1e-5));
    CHECK(lines[1].b.z == Approx(8.0f));
    CHECK(lines[7].b.x == Approx(12.0f));
    CHECK(lines[7].b.z == Approx(10.0f).margin(1e-5));
    for (const DebugLine& l : lines) {
        CHECK(l.a.y == 5.0f);
        CHECK(l.b.y == 5.0f);
        CHECK(l.color == 0xff00ff00u);
    }
}

TEST_CASE("DrawLinePop's ring sits two units back from the line's end", "[debugdraw]") {
    DebugCanvas canvas;
    canvas.line_pop({0, 0, 0}, {10, 0, 0}, 0xffffffffu);
    const auto& lines = canvas.lines();
    REQUIRE(lines.size() == 9);
    CHECK(lines[0].a.x == 0.0f);
    CHECK(lines[0].b.x == 10.0f);
    // Radius 1 about (8, 0, 0): from its +x side, (9, 0, 0)
    CHECK(lines[1].a.x == Approx(9.0f));
    CHECK(lines[1].a.z == Approx(0.0f));
}

TEST_CASE("The sim's scripts draw into its canvas, which the tick's end rolls over for a beat",
          "[debugdraw]") {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};
    osc::lua::register_moho_bindings(state, sim);
    osc::lua::register_sim_bindings(state, sim);
    REQUIRE(state.do_string("DrawLine({1, 2, 3}, {4, 5, 6}, 'ffff0000')").ok());
    REQUIRE(state.do_string("DrawCircle({0, 0, 0}, 3, 'ff00ff00')").ok());
    REQUIRE(sim.debug_canvas().lines().size() == 9);
    CHECK(sim.debug_canvas().lines()[0].color == 0xffff0000u);
    CHECK(sim.debug_lines().empty());

    // Moho's checks: three arguments, a number for the size, a colour SCR_DecodeColor knows
    CHECK_FALSE(state.do_string("DrawLine({1, 2, 3}, {4, 5, 6})").ok());
    CHECK_FALSE(state.do_string("DrawCircle({0, 0, 0}, 'big', 'ff00ff00')").ok());
    CHECK_FALSE(state.do_string("DrawLinePop({0, 0, 0}, {1, 0, 0}, 'no such colour')").ok());

    sim.tick();
    CHECK(sim.debug_canvas().lines().empty());
    REQUIRE(sim.debug_lines().size() == 9);
    // The UI's tick has both: the last tick's, then what was drawn since
    REQUIRE(state.do_string("DrawLine({0, 0, 0}, {0, 9, 0}, 'ffffffff')").ok());
    osc::sim::WorldSnapshot seen;
    osc::sim::capture_world(sim, seen);
    REQUIRE(seen.debug_lines.size() == 10);
    CHECK(seen.debug_lines[0].a.x == 1.0f);
    CHECK(seen.debug_lines[9].b.y == 9.0f);

    sim.tick();
    sim.tick(); // drawn nothing since: gone
    CHECK(sim.debug_lines().empty());
}
