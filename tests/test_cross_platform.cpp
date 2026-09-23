// A synthetic game (no game data) that must play identically on every
// platform. CI runs it on GCC, Clang and MSVC, so the pinned checksum below
// is a cross-platform lockstep check at the scale CI can run: movement and
// flight (heading trigonometry through osc::dmath) and the session's random
// stream all feed it.

#include <catch2/catch_test_macros.hpp>

#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <vector>

using osc::sim::SimState;
using osc::sim::Unit;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

struct Result {
    osc::u32 checksum = 0;
    int moved = 0; ///< units that ended far from where they began
    int flown = 0; ///< aircraft that climbed
    int alive = 0;
};

Result play_synthetic_game(osc::u32 ticks) {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_seed(20260923);
    sim.set_victory_condition("sandbox"); // no commanders here: nobody is eliminated
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");

    auto& rng = sim.random();
    std::vector<osc::u32> ids;
    std::vector<osc::sim::Vector3> start;
    for (int i = 0; i < 18; ++i) {
        auto u = std::make_unique<Unit>();
        u->set_army(i % 2);
        u->set_position({static_cast<osc::f32>(rng.next_double() * 400.0), 0.0f,
                         static_cast<osc::f32>(rng.next_double() * 400.0)});
        if (i % 3 == 0) { // aircraft: heading-based flight
            u->set_layer("Air");
            u->set_max_airspeed(20.0f);
            u->set_turn_rate_rad(1.5f);
            u->set_accel_rate(5.0f);
            u->set_climb_rate(4.0f);
            u->set_elevation_target(18.0f);
        } else {
            u->set_max_speed(static_cast<osc::f32>(4.0 + rng.next_double() * 3.0));
        }
        start.push_back(u->position());
        ids.push_back(sim.entity_registry().register_entity(std::move(u)));
    }

    for (osc::u32 t = 0; t < ticks; ++t) {
        if (t % 10 == 0) {
            const auto who =
                ids[static_cast<size_t>(rng.next_int(0, static_cast<osc::i64>(ids.size()) - 1))];
            osc::sim::UnitCommand move;
            move.type = osc::sim::CommandType::Move;
            move.target_pos = {static_cast<osc::f32>(rng.next_double() * 400.0), 0.0f,
                               static_cast<osc::f32>(rng.next_double() * 400.0)};
            sim.schedule_command(0, {who}, move, true);
        }
        sim.tick();
    }
    Result r;
    r.checksum = sim.compute_sync_checksum();
    for (size_t i = 0; i < ids.size(); ++i) {
        const auto* e = sim.entity_registry().find(ids[i]);
        if (!e) continue;
        ++r.alive;
        const auto& p = e->position();
        const float dx = p.x - start[i].x, dz = p.z - start[i].z;
        if (dx * dx + dz * dz > 25.0f * 25.0f) ++r.moved;
        if (p.y > 5.0f) ++r.flown;
    }
    return r;
}

} // namespace

TEST_CASE("A synthetic game reaches the same checksum on every platform",
          "[determinism][crossplatform]") {
    const Result first = play_synthetic_game(600);
    // It plays: units walk and aircraft climb and fly.
    INFO("alive " << first.alive << " moved " << first.moved << " flown " << first.flown);
    CHECK(first.alive == 18);
    CHECK(first.moved >= 8);
    CHECK(first.flown >= 4);
    // This platform agrees with itself...
    CHECK(play_synthetic_game(600).checksum == first.checksum);
    // ...and every platform CI builds on must reach this value. If it
    // changes, a rule or the sim's arithmetic changed: update it on purpose,
    // from a run on any one platform.
    INFO("checksum " << std::hex << first.checksum);
    CHECK(first.checksum == 0x49ed01dcu);
}
