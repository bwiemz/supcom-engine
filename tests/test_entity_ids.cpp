#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/command_codec.hpp"
#include "sim/entity.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/state_io.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "support/tick_until.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <memory>
#include <vector>

using osc::f32;
using osc::u32;
using osc::sim::CommandType;
using osc::sim::Entity;
using osc::sim::EntityId;
using osc::sim::EntityRegistry;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

std::unique_ptr<Entity> script_entity(int army) {
    auto e = std::make_unique<Entity>();
    e->set_army(army);
    return e;
}

u32 serial(EntityId id) {
    return id.moho() & 0xFFFFFu;
}

void tick(SimState& sim, int beats) {
    for (int i = 0; i < beats; ++i) {
        sim.tick();
    }
}

void flat(SimState& sim) {
    std::vector<osc::u16> heights(129 * 129, 1000);
    osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
    sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
    sim.build_pathfinding_grid();
    sim.add_army("ARMY_1", "ARMY_1");
    sim.set_fog_of_war("none");
    sim.set_victory_condition("sandbox");
}

Unit* walker(SimState& sim, f32 x, f32 z, f32 speed) {
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_max_speed(speed);
    u->set_position({x, 0.0f, z});
    u->set_max_health(100.0f);
    u->set_health(100.0f);
    Unit::Drive drive;
    drive.max_accel = 50.0f;
    u->set_drive(drive);
    Unit* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

} // namespace

TEST_CASE("A pool past 2^20 serials never hands out a live entity's id", "[sim][ids]") {
    EntityRegistry reg;
    const EntityId keeper_id = reg.register_entity(script_entity(1));
    const Entity* keeper = reg.find(keeper_id);
    REQUIRE(keeper_id.moho() == 0x501u << 20);

    for (u32 i = 0; i < (1u << 20); ++i) {
        reg.unregister_entity(reg.register_entity(script_entity(0)));
        if (i % 4096 == 0) {
            reg.collect_garbage();
        }
    }
    const EntityId id = reg.register_entity(script_entity(0));

    CHECK(reg.find(keeper_id) == keeper);
    CHECK(id.moho() == (0x501u << 20 | 1u));
    CHECK(reg.count() == 2);
}

TEST_CASE("A destroyed entity's Moho id is taken again 99 beats on, the least first",
          "[sim][ids]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    EntityRegistry& reg = sim.entity_registry();
    std::vector<EntityId> made;
    made.reserve(5);
    for (int i = 0; i < 5; ++i) {
        made.push_back(reg.register_entity(script_entity(0)));
    }
    reg.destroy(*reg.find(made[3]));
    reg.destroy(*reg.find(made[1]));
    tick(sim, 98);
    CHECK(serial(reg.register_entity(script_entity(0))) == 5);

    tick(sim, 1);
    CHECK(serial(reg.register_entity(script_entity(0))) == 1);
    CHECK(serial(reg.register_entity(script_entity(0))) == 3);
    CHECK(serial(reg.register_entity(script_entity(0))) == 6);
}

TEST_CASE("A dead entity's id never finds the one that takes its Moho id", "[sim][ids]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    EntityRegistry& reg = sim.entity_registry();
    const EntityId dead = reg.register_entity(script_entity(0));
    reg.destroy(*reg.find(dead));
    tick(sim, 99);

    const EntityId next = reg.register_entity(script_entity(0));
    CHECK(next.moho() == dead.moho());
    CHECK(next != dead);
    CHECK(reg.find(dead) == nullptr);
    CHECK(reg.find(next) != nullptr);
    CHECK(reg.live_id(dead.moho()) == next);
}

TEST_CASE("A queued order on a dead unit doesn't take the unit that gets its Moho id",
          "[sim][ids]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* ward = walker(sim, 50.0f, 50.0f, 0.0f);
    Unit* tank = walker(sim, 10.0f, 10.0f, 5.0f);
    UnitCommand move;
    move.type = CommandType::Move;
    move.target_pos = {120.0f, 0.0f, 10.0f};
    move.command_id = 1;
    UnitCommand guard;
    guard.type = CommandType::Guard;
    guard.target_id = ward->entity_id();
    guard.target_pos = ward->position();
    guard.command_id = 2;
    tank->push_command(move, true);
    tank->push_command(guard, false);
    const EntityId ward_id = ward->entity_id();
    sim.entity_registry().destroy(*ward);
    tick(sim, 99);

    const Unit* next = walker(sim, 60.0f, 60.0f, 0.0f);
    REQUIRE(next->entity_id().moho() == ward_id.moho());
    REQUIRE(tank->command_queue().size() == 2);
    osc::test::tick_until([&] { sim.tick(); }, "the move ends", 400,
                          [&] { return tank->command_queue().size() < 2; });
    sim.tick();
    CHECK(tank->command_queue().empty());
}

TEST_CASE("Entities come in Moho's id order, a reused id before the later ones", "[sim][ids]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    EntityRegistry& reg = sim.entity_registry();
    const EntityId first = reg.register_entity(script_entity(0));
    const EntityId later = reg.register_entity(script_entity(0));
    reg.destroy(*reg.find(first));
    tick(sim, 99);
    const EntityId reused = reg.register_entity(script_entity(0));

    REQUIRE(reused.moho() == first.moho());
    CHECK(reg.collect_in_rect(-1, -1, 1, 1) == std::vector<EntityId>{reused, later});
}

TEST_CASE("A saved game takes a held Moho id again on the beat the unsaved one does",
          "[sim][ids][save]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    EntityRegistry& reg = sim.entity_registry();
    for (int i = 0; i < 3; ++i) {
        reg.register_entity(script_entity(0));
    }
    const u32 second = 0x500u << 20 | 1u;
    reg.destroy(*reg.find(reg.live_id(second)));
    tick(sim, 50);

    LuaGuard g2;
    SimState loaded(g2.L, nullptr);
    REQUIRE(osc::sim::load_sim_state(loaded, osc::sim::save_sim_state(sim)).empty());
    for (SimState* s : {&sim, &loaded}) {
        tick(*s, 48);
        CHECK(serial(s->entity_registry().register_entity(script_entity(0))) == 3);
        tick(*s, 1);
        CHECK(serial(s->entity_registry().register_entity(script_entity(0))) == 1);
    }
    CHECK(sim.entity_registry().live_id(second) == loaded.entity_registry().live_id(second));
}

TEST_CASE("A command carries entity keys past 32 bits", "[sim][ids][replay]") {
    osc::sim::ScheduledCommand c;
    c.unit_ids = {EntityId::of(0x10500003u, 7), EntityId::of(5, 2)};
    c.command.target_id = EntityId::of(0x00200001u, 3);
    std::vector<osc::u8> bytes;
    osc::sim::ByteWriter w(bytes);
    osc::sim::write_command(w, c);

    osc::sim::ScheduledCommand back;
    osc::sim::ByteReader r(bytes);
    REQUIRE(osc::sim::read_command(r, back));
    CHECK(back.unit_ids == c.unit_ids);
    CHECK(back.command.target_id == c.command.target_id);
}
