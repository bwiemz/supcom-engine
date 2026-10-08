// A guard takes on enemies near it and goes home after, as Moho's
// CUnitGuardTask does; its fights, and a patrol's, are leashed at
// AI.GuardReturnRadius (CAcquireTargetTask::CheckTargetGuardExempt). See
// docs/plans/2026-10-05-guard-engagement-design.md.

#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/army_brain.hpp"
#include "sim/manipulator.hpp"
#include "sim/prop.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using osc::f32;
using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

void flat(SimState& sim) {
    std::vector<osc::u16> heights(129 * 129, 1000);
    osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
    sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
    sim.build_pathfinding_grid();
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.set_fog_of_war("none");
    sim.set_victory_condition("sandbox");
}

Unit* walker(SimState& sim, int army, f32 x, f32 z, f32 speed = 5.0f) {
    auto u = std::make_unique<Unit>();
    u->set_army(army);
    u->set_max_speed(speed);
    u->set_position({x, 0.0f, z});
    u->set_max_health(100.0f);
    u->set_health(100.0f);
    Unit::Drive drive;
    drive.max_accel = 50.0f;
    u->set_drive(drive);
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

/// A walker with a gun of range 5 (no damage: what it shoots at lives).
Unit* armed(SimState& sim, int army, f32 x, f32 z) {
    Unit* u = walker(sim, army, x, z);
    auto gun = std::make_unique<osc::sim::Weapon>();
    gun->max_range = 5.0f;
    u->add_weapon(std::move(gun));
    u->set_guard_scan_radius(20.0f);
    return u;
}

Unit* still(SimState& sim, int army, f32 x, f32 z) {
    return walker(sim, army, x, z, 0.0f);
}

UnitCommand guard(const Unit* of, osc::u32 id, osc::sim::Vector3 point = {}) {
    UnitCommand c;
    c.type = CommandType::Guard;
    c.target_id = of ? of->entity_id() : 0;
    c.target_pos = of ? of->position() : point;
    c.command_id = id;
    return c;
}

/// The fight a guard broke off for, at the head of its queue, or null.
const UnitCommand* fight(const Unit& u) {
    const auto& q = u.command_queue();
    return !q.empty() && q.front().from_guard && q.front().type == CommandType::Attack ? &q.front()
                                                                                       : nullptr;
}

f32 dist(const Unit& a, const Unit& b) {
    return std::hypot(a.position().x - b.position().x, a.position().z - b.position().z);
}

} // namespace

TEST_CASE("A guard takes on an enemy within its GuardScanRadius of itself, not one beyond",
          "[guard]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* ward = still(sim, 0, 50.0f, 50.0f);
    Unit* tank = armed(sim, 0, 55.0f, 50.0f);
    Unit* far = still(sim, 1, 80.0f, 50.0f); // 25 from the tank
    tank->push_command(guard(ward, 7), true);
    for (int t = 0; t < 30; ++t) sim.tick();
    CHECK(fight(*tank) == nullptr);

    Unit* near = still(sim, 1, 70.0f, 55.0f); // 15.8 from the tank
    const UnitCommand* f = nullptr;
    for (int t = 0; t < 10 && !f; ++t) {
        sim.tick();
        f = fight(*tank);
    }
    REQUIRE(f);
    CHECK(f->target_id == near->entity_id());
    CHECK(f->command_id == 7);
    CHECK(f->leash_anchor_id == ward->entity_id());
    // Fighting, it still guards.
    CHECK(tank->guarded_unit_id() == ward->entity_id());
    CHECK(tank->guard_order() != nullptr);
    (void)far;
}

TEST_CASE("Back from its fight, a guard goes home before it looks about again", "[guard]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* ward = still(sim, 0, 30.0f, 50.0f);
    Unit* tank = armed(sim, 0, 35.0f, 50.0f);
    Unit* enemy = still(sim, 1, 50.0f, 50.0f);
    tank->push_command(guard(ward, 1), true);
    for (int t = 0; t < 10 && !fight(*tank); ++t) sim.tick();
    REQUIRE(fight(*tank));
    for (int t = 0; t < 60; ++t) sim.tick(); // up to it, in range
    CHECK(dist(*tank, *enemy) <= 5.5f);

    // Its target gone, the guard is back at the head, on its way home; a new
    // enemy within its scan of where it stands, but not home, waits.
    sim.entity_registry().unregister_entity(enemy->entity_id());
    Unit* other = still(sim, 1, 62.0f, 50.0f);
    sim.tick();
    REQUIRE(!tank->command_queue().empty());
    CHECK(tank->command_queue().front().type == CommandType::Guard);
    CHECK(tank->command_queue().front().guard_returning);
    bool home = false;
    for (int t = 0; t < 100 && !home; ++t) {
        sim.tick();
        CHECK(fight(*tank) == nullptr);
        home = !tank->command_queue().front().guard_returning;
    }
    CHECK(home);
    CHECK(dist(*tank, *ward) <= 10.5f); // its footprint (0) and half its scan
    CHECK(dist(*tank, *other) > 20.0f); // home, the other is out of its scan
}

TEST_CASE("A guard's fight ends at GuardReturnRadius from what it guards, once it has been in "
          "range",
          "[guard]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* ward = still(sim, 0, 10.0f, 50.0f);
    Unit* tank = armed(sim, 0, 15.0f, 50.0f);
    tank->set_guard_return_radius(30.0f);
    // It reaches the runner (9 off, slower) well within the leash, then
    // follows it out until it is 30 from what it guards.
    Unit* runner = walker(sim, 1, 24.0f, 50.0f, 3.0f);
    UnitCommand away;
    away.type = CommandType::Move;
    away.target_pos = {120.0f, 0.0f, 50.0f};
    away.command_id = 2;
    runner->push_command(away, true);
    tank->push_command(guard(ward, 1), true);
    bool armed_once = false;
    bool ended = false;
    f32 farthest = 0;
    for (int t = 0; t < 400 && !ended; ++t) {
        sim.tick();
        farthest = std::max(farthest, dist(*tank, *ward));
        if (const UnitCommand* f = fight(*tank)) armed_once = armed_once || f->leash_armed;
        ended = armed_once && !fight(*tank);
    }
    CHECK(armed_once);
    CHECK(ended);
    CHECK(farthest <= 32.0f);                         // GRR 30, and its stop and turn
    CHECK(runner->position().x > tank->position().x); // it got away
    // It goes home, and lets the runner go (beyond its scan).
    for (int t = 0; t < 200; ++t) sim.tick();
    CHECK(fight(*tank) == nullptr);
    CHECK(dist(*tank, *ward) <= 10.5f);
}

TEST_CASE("An engineer guarding an engineer helps, and takes on no enemies", "[guard]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* ward = still(sim, 0, 50.0f, 50.0f);
    ward->add_category("ENGINEER");
    Unit* acu = armed(sim, 0, 55.0f, 50.0f);
    acu->add_category("ENGINEER");
    still(sim, 1, 65.0f, 50.0f);
    acu->push_command(guard(ward, 1), true);
    for (int t = 0; t < 60; ++t) {
        sim.tick();
        CHECK(fight(*acu) == nullptr);
    }

    // Guarding a tank, the same engineer does.
    Unit* tank = still(sim, 0, 55.0f, 45.0f);
    acu->push_command(guard(tank, 2), true);
    for (int t = 0; t < 10 && !fight(*acu); ++t) sim.tick();
    CHECK(fight(*acu) != nullptr);
}

TEST_CASE("A guard of a point goes there and takes on enemies near it", "[guard]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* tank = armed(sim, 0, 10.0f, 10.0f);
    Unit* enemy = still(sim, 1, 70.0f, 60.0f);
    tank->push_command(guard(nullptr, 1, {60.0f, 0.0f, 60.0f}), true);
    const UnitCommand* f = nullptr;
    for (int t = 0; t < 300 && !f; ++t) {
        sim.tick();
        f = fight(*tank);
    }
    REQUIRE(f);
    CHECK(f->target_id == enemy->entity_id());
    CHECK(f->leash_anchor_id == 0);
    CHECK(f->leash_anchor_pos.x == 60.0f);
    CHECK(f->leash_anchor_pos.z == 60.0f);
    CHECK(tank->guarded_unit_id() == 0);
    CHECK(tank->guard_order() != nullptr);

    sim.entity_registry().unregister_entity(enemy->entity_id());
    for (int t = 0; t < 200; ++t) sim.tick();
    REQUIRE(!tank->command_queue().empty());
    CHECK(tank->command_queue().front().type == CommandType::Guard);
    CHECK(std::hypot(tank->position().x - 60.0f, tank->position().z - 60.0f) <= 2.5f);
}

TEST_CASE("A guard leaves alone what its own queue captures", "[guard]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* ward = still(sim, 0, 50.0f, 50.0f);
    Unit* tank = armed(sim, 0, 55.0f, 50.0f);
    Unit* prize = still(sim, 1, 65.0f, 50.0f);
    tank->push_command(guard(ward, 1), true);
    UnitCommand capture;
    capture.type = CommandType::Capture;
    capture.target_id = prize->entity_id();
    capture.target_pos = prize->position();
    capture.command_id = 2;
    tank->push_command(capture, false);
    for (int t = 0; t < 60; ++t) {
        sim.tick();
        CHECK(fight(*tank) == nullptr);
    }
}

namespace {

int lua_ref_of(lua_State* L, const char* source) {
    const std::string code = std::string("return ") + source;
    REQUIRE(luaL_loadbuffer(L, code.c_str(), code.size(), "t") == 0);
    REQUIRE(lua_pcall(L, 0, 1, 0) == 0);
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

void run_lua(lua_State* L, const char* code) {
    REQUIRE(luaL_loadbuffer(L, code, std::strlen(code), "c") == 0);
    REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
}

} // namespace

TEST_CASE("Engineers assisting a shield generator regenerate its bubble while it is on",
          "[guard]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    run_lua(g.L, "on = true\n"
                 "__blueprints = { gen = { Economy = { BuildTime = 100 },"
                 " Defense = { Shield = { RegenAssistMult = 60 } } } }");
    Unit* gen = still(sim, 0, 50.0f, 50.0f);
    gen->set_unit_id("gen");
    gen->add_category("SHIELD");
    gen->set_lua_table_ref(lua_ref_of(g.L, "{ ShieldIsOn = function() return on end }"));
    auto bubble_owned = std::make_unique<osc::sim::Shield>();
    osc::sim::Shield* bubble = bubble_owned.get();
    bubble->set_army(0);
    bubble->set_max_health(9000.0f);
    bubble->set_health(1000.0f);
    sim.entity_registry().register_entity(std::move(bubble_owned));
    bubble->set_lua_table_ref(lua_ref_of(g.L, "{ RegenRate = 120 }"));
    gen->set_focus_entity_id(bubble->entity_id());

    Unit* eng = walker(sim, 0, 53.0f, 50.0f);
    eng->add_category("ENGINEER");
    eng->add_category("REPAIR");
    eng->set_build_rate(5.0f);
    eng->set_max_build_distance(5.0f);
    eng->push_command(guard(gen, 1), true);
    for (int t = 0; t < 10; ++t) {
        sim.tick();
    }
    CHECK(eng->repair_target_id() == gen->entity_id());
    CHECK(bubble->health() > 1005.0f);

    const f32 off_at = bubble->health();
    run_lua(g.L, "on = false");
    for (int t = 0; t < 10; ++t) {
        sim.tick();
    }
    CHECK(eng->repair_target_id() == 0);
    CHECK(bubble->health() == off_at);

    run_lua(g.L, "on = true");
    gen->set_health(50.0f);
    for (int t = 0; t < 2; ++t) {
        sim.tick();
    }
    const f32 before = bubble->health();
    sim.tick();
    CHECK(std::abs(bubble->health() - before - 0.5f) < 1e-3f);
}
