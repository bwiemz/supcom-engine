// No Rush option tests: units are confined near their army start for the
// no-rush period, via both the scheduler (goal clamp) and a per-tick pin.

#include <catch2/catch_test_macros.hpp>

#include "sim/army_brain.hpp"
#include "sim/command_scheduler.hpp"
#include "sim/manipulator.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <cmath>
#include <memory>

using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;
using osc::sim::Vector3;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

osc::u32 spawn_mover(SimState& sim, int army, const Vector3& pos, osc::f32 speed) {
    auto u = std::make_unique<Unit>();
    u->set_army(army);
    u->set_max_speed(speed);
    u->set_position(pos);
    return sim.entity_registry().register_entity(std::move(u));
}

osc::f32 dist_xz(const Vector3& a, const Vector3& b) {
    osc::f32 dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

UnitCommand move_to(osc::f32 x, osc::f32 z) {
    UnitCommand c;
    c.type = CommandType::Move;
    c.target_pos = {x, 0.0f, z};
    return c;
}

} // namespace

TEST_CASE("No Rush is inactive by default and after its window", "[norush]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    CHECK_FALSE(sim.no_rush_active());

    sim.set_no_rush(1.0f, 50.0f); // 1 second
    CHECK(sim.no_rush_active());
    for (int i = 0; i < 12; ++i) sim.tick(); // > 1.0s of game time
    CHECK_FALSE(sim.no_rush_active());
}

TEST_CASE("No Rush pins a straying unit at the boundary", "[norush]") {
    const Vector3 origin{0.0f, 0.0f, 0.0f};
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.get_army(0)->set_start_position({0.0f, 0.0f, 0.0f});
    sim.set_no_rush(100.0f, 50.0f);

    osc::u32 id = spawn_mover(sim, 0, {0.0f, 0.0f, 0.0f}, 10.0f);
    auto* u = static_cast<Unit*>(sim.entity_registry().find(id));
    // Direct order (as an AI would issue) toward a far target.
    u->push_command(move_to(1000.0f, 0.0f), true);

    for (int i = 0; i < 40; ++i) sim.tick();
    CHECK(dist_xz(u->position(), origin) <= 50.5f);
}

TEST_CASE("No Rush clamps a scheduled move goal to the zone", "[norush]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.get_army(0)->set_start_position({0.0f, 0.0f, 0.0f});
    sim.set_no_rush(100.0f, 50.0f);

    osc::u32 id = spawn_mover(sim, 0, {0.0f, 0.0f, 0.0f}, 10.0f);
    sim.schedule_command(0, {id}, move_to(500.0f, 0.0f), true);
    sim.tick(); // dispatch clamps the goal

    auto* u = static_cast<Unit*>(sim.entity_registry().find(id));
    REQUIRE_FALSE(u->command_queue().empty());
    // Goal clamped onto the radius circle (x ~= 50).
    CHECK(u->command_queue().front().target_pos.x <= 50.1f);
    CHECK(u->command_queue().front().target_pos.x >= 49.9f);
}

TEST_CASE("Units confined during No Rush are freed once it expires", "[norush]") {
    const Vector3 origin{0.0f, 0.0f, 0.0f};
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.get_army(0)->set_start_position({0.0f, 0.0f, 0.0f});
    sim.set_no_rush(1.0f, 50.0f); // 1 second window

    osc::u32 id = spawn_mover(sim, 0, {0.0f, 0.0f, 0.0f}, 20.0f);
    auto* u = static_cast<Unit*>(sim.entity_registry().find(id));
    u->push_command(move_to(1000.0f, 0.0f), true);

    for (int i = 0; i < 8; ++i) sim.tick();     // inside the window
    CHECK(dist_xz(u->position(), origin) <= 50.5f);

    for (int i = 0; i < 60; ++i) sim.tick();     // window expired → free to leave
    CHECK(dist_xz(u->position(), origin) > 50.5f);
}

TEST_CASE("No Rush confines each army to its own start", "[norush]") {
    const Vector3 base2{200.0f, 0.0f, 0.0f};
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.get_army(0)->set_start_position({0.0f, 0.0f, 0.0f});
    sim.get_army(1)->set_start_position({200.0f, 0.0f, 0.0f});
    sim.set_no_rush(100.0f, 50.0f);

    osc::u32 id = spawn_mover(sim, 1, {200.0f, 0.0f, 0.0f}, 10.0f);
    auto* u = static_cast<Unit*>(sim.entity_registry().find(id));
    u->push_command(move_to(0.0f, 0.0f), true); // head toward the other base

    for (int i = 0; i < 40; ++i) sim.tick();
    CHECK(dist_xz(u->position(), base2) <= 50.5f);
    CHECK(u->position().x >= 149.5f); // never crossed toward the enemy base
}

TEST_CASE("Civilian units are exempt from No Rush", "[norush]") {
    const Vector3 origin{0.0f, 0.0f, 0.0f};
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.add_army("CIVILIAN", "CIVILIAN"); // civilian army 0
    sim.get_army(0)->set_start_position({0.0f, 0.0f, 0.0f});
    sim.set_no_rush(100.0f, 50.0f);

    osc::u32 id = spawn_mover(sim, 0, {0.0f, 0.0f, 0.0f}, 20.0f);
    auto* u = static_cast<Unit*>(sim.entity_registry().find(id));
    u->push_command(move_to(1000.0f, 0.0f), true);

    for (int i = 0; i < 60; ++i) sim.tick();
    CHECK(dist_xz(u->position(), origin) > 50.5f); // wildlife roams freely
}
