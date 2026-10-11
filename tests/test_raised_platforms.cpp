#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <string>
#include <utility>
#include <vector>

using Catch::Approx;
using osc::f32;
using osc::sim::CollisionShape;
using osc::sim::CollisionShapeType;
using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

constexpr f32 kGround = 1000.0f / 128.0f;

void flat(SimState& sim) {
    std::vector<osc::u16> heights(129 * 129, 1000);
    osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
    sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
    sim.build_pathfinding_grid();
    sim.add_army("ARMY_1", "ARMY_1");
    sim.set_fog_of_war("none");
    sim.set_victory_condition("sandbox");
}

// UEB0101's deck, then a ramp rising 1 along x beside it.
std::vector<f32> decks() {
    return {-1.1f, -1.6f, 0.5f, 1.1f, -1.6f, 0.5f, -1.1f, 1.9f, 0.5f, 1.1f, 1.9f, 0.5f,
            1.1f,  -1.6f, 0.0f, 2.1f, -1.6f, 1.0f, 1.1f,  1.9f, 0.0f, 2.1f, 1.9f, 1.0f};
}

Unit* factory(SimState& sim, f32 x, f32 z) {
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_position({x, kGround, z});
    u->set_max_health(100.0f);
    u->set_health(100.0f);
    u->set_size_xz(4.2f, 4.4f);
    u->set_size_y(0.6f);
    CollisionShape box;
    box.type = CollisionShapeType::BOX;
    box.cy = 0.3f;
    box.sx = 2.1f;
    box.sy = 0.3f;
    box.sz = 2.2f;
    u->set_default_collision_shape(box);
    u->set_raised_platforms(decks());
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

Unit* tank(SimState& sim, f32 x, f32 z) {
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_max_speed(1.0f);
    u->set_motion_type("RULEUMT_Land");
    u->set_position({x, kGround, z});
    u->set_max_health(100.0f);
    u->set_health(100.0f);
    u->set_size_xz(0.6f, 0.8f);
    u->set_size_y(0.4f);
    Unit::Drive drive;
    drive.max_accel = 50.0f;
    u->set_drive(drive);
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

} // namespace

TEST_CASE("A land unit on a factory's deck stands on its raised platform", "[platforms]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    factory(sim, 64.0f, 64.0f);
    Unit* u = tank(sim, 64.0f, 64.5f);
    const auto* terrain = sim.terrain();
    CHECK(u->ground_y(terrain, 64.0f, 64.5f) == Approx(kGround + 0.5f));
    CHECK(u->ground_y(terrain, 65.6f, 64.5f) == Approx(kGround + 0.5f));
    CHECK(u->ground_y(terrain, 64.0f, 70.0f) == kGround);
    Unit* away = tank(sim, 64.0f, 72.0f);
    CHECK(away->ground_y(terrain, 64.0f, 64.5f) == kGround);
}

TEST_CASE("A unit driving off a deck rides it, and a dying factory's deck is gone", "[platforms]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* f = factory(sim, 64.0f, 64.0f);
    Unit* u = tank(sim, 64.0f, 64.0f);
    UnitCommand move;
    move.type = CommandType::Move;
    move.target_pos = {64.0f, 0.0f, 80.0f};
    u->push_command(move, true);
    sim.tick();
    sim.tick();
    REQUIRE(u->position().z > 64.0f);
    REQUIRE(u->position().z < 65.5f);
    CHECK(u->position().y == Approx(kGround + 0.5f));
    f->begin_dying();
    sim.tick();
    CHECK(u->position().y == kGround);
}
