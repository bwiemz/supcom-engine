// No Rush: while it runs, an army's orders keep to its zone.

#include <catch2/catch_test_macros.hpp>

#include "sim/army_brain.hpp"
#include "sim/command_scheduler.hpp"
#include "sim/manipulator.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "support/tick_until.hpp"

extern "C" {
#include <lua.h>
}

#include <cmath>
#include <memory>

using osc::sim::CommandType;
using osc::sim::EntityId;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;
using osc::sim::Vector3;
using osc::test::tick_until;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

EntityId spawn_mover(SimState& sim, int army, const Vector3& pos, osc::f32 speed) {
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

TEST_CASE("No Rush holds no unit at its zone's edge", "[norush]") {
    const Vector3 origin{0.0f, 0.0f, 0.0f};
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.get_army(0)->set_start_position({0.0f, 0.0f, 0.0f});
    sim.set_no_rush(100.0f, 50.0f);

    EntityId id = spawn_mover(sim, 0, {0.0f, 0.0f, 0.0f}, 10.0f);
    auto* u = static_cast<Unit*>(sim.entity_registry().find(id));
    u->push_command(move_to(1000.0f, 0.0f), true);
    tick_until([&] { sim.tick(); }, "the unit walks out of the zone", 200,
               [&] { return dist_xz(u->position(), origin) > 60.0f; });
    EntityId outside = spawn_mover(sim, 0, {90.0f, 0.0f, 0.0f}, 10.0f);
    for (int i = 0; i < 20; ++i) {
        sim.tick();
    }
    CHECK(sim.entity_registry().find(outside)->position().x == 90.0f);
}

TEST_CASE("No Rush refuses a scheduled move out of the zone, and takes one in it", "[norush]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.get_army(0)->set_start_position({0.0f, 0.0f, 0.0f});
    sim.set_no_rush(100.0f, 50.0f);

    EntityId id = spawn_mover(sim, 0, {0.0f, 0.0f, 0.0f}, 10.0f);
    sim.schedule_command(0, {id}, move_to(500.0f, 0.0f), true);
    sim.tick();
    auto* u = static_cast<Unit*>(sim.entity_registry().find(id));
    CHECK(u->command_queue().empty());

    sim.schedule_command(0, {id}, move_to(40.0f, 0.0f), true);
    sim.tick();
    REQUIRE_FALSE(u->command_queue().empty());
    CHECK(u->command_queue().front().target_pos.x == 40.0f);
}

TEST_CASE("Each army's No Rush zone is about its start and the map's offset", "[norush]") {
    struct Case {
        const char* what;
        int army;
        Vector3 to;
        bool taken;
    };
    const Case cases[] = {
        {"near its start, far from the centre", 0, {0.0f, 0.0f, -45.0f}, false},
        {"far from its start, near the centre", 0, {15.0f, 0.0f, 85.0f}, true},
        {"just in", 0, {63.0f, 0.0f, 40.0f}, true},
        {"just out", 0, {67.0f, 0.0f, 40.0f}, false},
        {"the other army's, at its own start", 1, {200.0f, 0.0f, 0.0f}, true},
        {"the other army's, at the first's centre", 1, {15.0f, 0.0f, 40.0f}, false},
    };
    for (const Case& c : cases) {
        LuaGuard g;
        SimState sim(g.L, nullptr);
        sim.set_victory_condition("sandbox");
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        sim.get_army(0)->set_start_position({0.0f, 0.0f, 0.0f});
        sim.get_army(0)->set_no_rush_offset({15.0f, 0.0f, 40.0f});
        sim.get_army(1)->set_start_position({200.0f, 0.0f, 0.0f});
        sim.set_no_rush(100.0f, 50.0f);
        const Vector3 at = c.army == 0 ? Vector3{15.0f, 0.0f, 40.0f} : Vector3{200.0f, 0.0f, 0.0f};
        EntityId id = spawn_mover(sim, c.army, at, 10.0f);
        INFO(c.what);
        CHECK((sim.route_command({id}, move_to(c.to.x, c.to.z), true) != 0) == c.taken);
    }
}

TEST_CASE("An order out of the zone is taken once No Rush ends", "[norush]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.get_army(0)->set_start_position({0.0f, 0.0f, 0.0f});
    sim.set_no_rush(1.0f, 50.0f);
    EntityId id = spawn_mover(sim, 0, {0.0f, 0.0f, 0.0f}, 10.0f);
    CHECK(sim.route_command({id}, move_to(500.0f, 0.0f), true) == 0);
    tick_until([&] { sim.tick(); }, "No Rush ends", 30, [&] { return !sim.no_rush_active(); });
    CHECK(sim.route_command({id}, move_to(500.0f, 0.0f), true) != 0);
}
