// The render benchmark (M223b): where the pinned game's battle is, and its GPU time by pass.

#include <catch2/catch_test_macros.hpp>

#include "app/app_internal.hpp"
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
#include <string>
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
    // Monotonic timestamps: the passes sum to the frame, start to end.
    CHECK(ms[static_cast<size_t>(Pass::Other)] + ms[static_cast<size_t>(Pass::Shadow)] +
              ms[static_cast<size_t>(Pass::Main)] ==
          900.0);

    // A counter of 8 valid bits wraps between two marks; one that steps back
    // counts nothing rather than most of its range.
    CHECK(Queries::ticks_between(250, 4, 0xff) == 10);
    CHECK(Queries::ticks_between(10, 4, 0xff) == 0);
    CHECK(Queries::ticks_between(10, 4, ~0ull) == 0);
}

TEST_CASE("A slot's timestamps: start and end alone unless timing passes", "[bench]") {
    using Pass = osc::renderer::GpuFrameQueries::Pass;
    using Queries = osc::renderer::GpuFrameQueries;
    Queries::Marks marks;

    // Not timing: the two queries a frame always had, and marks write none.
    CHECK(marks.begin(false) == 2);
    CHECK_FALSE(marks.mark(Pass::Shadow).has_value());
    CHECK(marks.end() == 1);
    CHECK(marks.written() == 2);
    CHECK(marks.passes().size() == 1);

    // Timing: every query reset, each mark the next, up to kMaxMarks.
    CHECK(marks.begin(true) == Queries::kQueriesPerSlot);
    CHECK(marks.mark(Pass::Shadow) == 1u);
    CHECK(marks.mark(Pass::Main) == 2u);
    for (osc::u32 i = 3; i <= Queries::kMaxMarks; ++i) REQUIRE(marks.mark(Pass::Bloom) == i);
    CHECK_FALSE(marks.mark(Pass::Composite).has_value());
    CHECK(marks.end() == Queries::kQueriesPerSlot - 1);
    CHECK(marks.written() == Queries::kQueriesPerSlot);
    CHECK(marks.passes()[1] == Pass::Shadow);
    CHECK(marks.passes().back() == Pass::Bloom);

    // The slot comes round again: last frame's marks are gone.
    CHECK(marks.begin(true) == Queries::kQueriesPerSlot);
    CHECK(marks.passes().size() == 1);
    CHECK(marks.passes()[0] == Pass::Other);
    CHECK(marks.mark(Pass::Normals) == 1u);
}

TEST_CASE("The render bench reports every pass's GPU time", "[bench]") {
    using Queries = osc::renderer::GpuFrameQueries;
    // 20 frames, main taking 1..20 ms: nearest-rank p50 10, p95 19.
    std::vector<Queries::Frame> frames(20);
    for (size_t i = 0; i < frames.size(); ++i) {
        frames[i].timed = true;
        frames[i].passes_timed = true;
        frames[i].pass_ms[static_cast<size_t>(Queries::Pass::Main)] = 1.0 + static_cast<double>(i);
        frames[i].pass_ms[static_cast<size_t>(Queries::Pass::Shadow)] = 0.5;
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
    CHECK(passes["main"]["p50_ms"] == 10.0);
    CHECK(passes["main"]["p95_ms"] == 19.0);
    CHECK(passes["shadow"]["p95_ms"] == 0.5);
    CHECK(passes["bloom"]["p95_ms"] == 0.0);

    // Frames timed whole, not by pass (no --gpu-passes, or a device without
    // timestamps): none.
    for (auto& f : frames) f.passes_timed = false;
    CHECK(osc::app::gpu_passes_json(frames).is_null());
}

TEST_CASE("--gpu-passes asks the render bench to time each pass", "[bench]") {
    const auto parse = [](std::vector<std::string> args) {
        args.insert(args.begin(), "opensupcom");
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        return osc::app::parse_options(static_cast<int>(argv.size()), argv.data(), {});
    };
    const auto split = parse({"--render-bench", "report.json", "--gpu-passes"});
    REQUIRE(split);
    CHECK(split->gpu_passes);
    const auto whole = parse({"--render-bench", "report.json"});
    REQUIRE(whole);
    CHECK_FALSE(whole->gpu_passes);
}
