// The render benchmark (M223b): where the pinned game's battle is, and its GPU time by pass.

#include <catch2/catch_test_macros.hpp>

#include "app/render_bench.hpp"
#include "sim/army_brain.hpp"
#include "sim/manipulator.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <nlohmann/json.hpp>

extern "C" {
#include <lua.h>
}

#include <memory>
#include <vector>

using osc::app::busiest_battle;
using osc::app::RenderBench;
using osc::sim::SimState;
using osc::sim::Unit;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

void unit_at(SimState& sim, osc::i32 army, osc::f32 x, osc::f32 z) {
    auto u = std::make_unique<Unit>();
    u->set_army(army);
    u->set_position({x, 0.0f, z});
    sim.entity_registry().register_entity(std::move(u));
}

} // namespace

TEST_CASE("The render bench's battle is the busiest square two armies share", "[bench]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.add_army("NEUTRAL_CIVILIAN", "NEUTRAL_CIVILIAN");
    sim.army_at(2)->set_civilian(true);

    // No square with two armies: none.
    for (int i = 0; i < 9; ++i) unit_at(sim, 0, 10.0f + i, 10.0f); // army 1's base
    CHECK_FALSE(busiest_battle(sim).has_value());
    // A civilian beside an army is no battle.
    unit_at(sim, 2, 12.0f, 12.0f);
    CHECK_FALSE(busiest_battle(sim).has_value());

    // Two fights: 3 units in (64..128, 0..64), 4 in (0..64, 128..192).
    unit_at(sim, 0, 70.0f, 5.0f);
    unit_at(sim, 1, 71.0f, 5.0f);
    unit_at(sim, 1, 72.0f, 5.0f);
    for (int i = 0; i < 2; ++i) unit_at(sim, 0, 5.0f + i, 130.0f);
    for (int i = 0; i < 2; ++i) unit_at(sim, 1, 5.0f + i, 140.0f);
    auto at = busiest_battle(sim);
    REQUIRE(at.has_value());
    CHECK(at->x == 32.0f); // the square's centre
    CHECK(at->z == 160.0f);

    // A tie goes to the lower row: another 3 at (64..128, 64..128) doesn't
    // move it, and neither square beats the 4.
    unit_at(sim, 0, 70.0f, 70.0f);
    unit_at(sim, 1, 71.0f, 70.0f);
    unit_at(sim, 1, 72.0f, 70.0f);
    unit_at(sim, 0, 73.0f, 5.0f); // the first fight now has 4 too, on a lower row
    at = busiest_battle(sim);
    REQUIRE(at.has_value());
    CHECK(at->x == 96.0f);
    CHECK(at->z == 32.0f);
}

TEST_CASE("The render bench's scenes by name", "[bench]") {
    CHECK(RenderBench::scene_named("battle") == RenderBench::Scene::Battle);
    CHECK(RenderBench::scene_named("late") == RenderBench::Scene::Late);
    CHECK(RenderBench::scene_named("strategic") == RenderBench::Scene::Strategic);
    CHECK_FALSE(RenderBench::scene_named("Battle").has_value());
}

TEST_CASE("A frame's GPU time splits by the pass each mark starts", "[bench]") {
    using Pass = osc::renderer::GpuFrameQueries::Pass;
    using Queries = osc::renderer::GpuFrameQueries;
    // A tick of a millisecond: start, shadow, main, a second shadow (a split
    // view's), end.
    const std::vector<osc::u64> ticks{100, 150, 400, 410, 1000};
    const std::vector<Pass> passes{Pass::Other, Pass::Shadow, Pass::Main, Pass::Shadow};
    const Queries::PassTimes ms = Queries::split(ticks, passes, 1.0e6, ~0ull);
    CHECK(ms[static_cast<size_t>(Pass::Other)] == 50.0);
    CHECK(ms[static_cast<size_t>(Pass::Shadow)] == 840.0); // 250 + 590
    CHECK(ms[static_cast<size_t>(Pass::Main)] == 10.0);
    CHECK(ms[static_cast<size_t>(Pass::Bloom)] == 0.0);

    // A counter of 8 valid bits wraps between two marks.
    const std::vector<osc::u64> wrapped{250, 4};
    const std::vector<Pass> one{Pass::Composite};
    CHECK(Queries::split(wrapped, one, 1.0e6, 0xff)[static_cast<size_t>(Pass::Composite)] == 10.0);
}

TEST_CASE("The render bench reports every pass's GPU time", "[bench]") {
    using Queries = osc::renderer::GpuFrameQueries;
    std::vector<Queries::Frame> frames(4);
    for (size_t i = 0; i < frames.size(); ++i) {
        frames[i].timed = true;
        frames[i].pass_ms[static_cast<size_t>(Queries::Pass::Main)] = 1.0 + static_cast<double>(i);
    }
    const nlohmann::json passes = osc::app::gpu_passes_json(frames);
    REQUIRE(passes.is_object());
    CHECK(passes.size() == Queries::kPasses);
    for (const char* name : Queries::kPassNames) {
        INFO(name);
        REQUIRE(passes.contains(name));
        CHECK(passes[name].contains("p50_ms"));
        CHECK(passes[name].contains("p95_ms"));
    }
    CHECK(passes["main"]["max_ms"] == 4.0);
    CHECK(passes["shadow"]["max_ms"] == 0.0);

    // A device without timestamps: none.
    for (auto& f : frames) f.timed = false;
    CHECK(osc::app::gpu_passes_json(frames).is_null());
}
