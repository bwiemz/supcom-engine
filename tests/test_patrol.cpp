#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "renderer/input_handler.hpp"
#include "sim/army_brain.hpp"
#include "sim/manipulator.hpp"
#include "sim/navigator.hpp"
#include "sim/prop.hpp"
#include "sim/prop_script.hpp"
#include "sim/saved_game.hpp"
#include "sim/sim_snapshot.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <vector>

using osc::sim::CommandType;
using osc::sim::Prop;
using osc::sim::SimState;
using osc::sim::Unit;

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
}

Unit* walker(SimState& sim, osc::f32 x, osc::f32 z) {
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_max_speed(5.0f);
    u->set_position({x, 0.0f, z});
    Unit::Drive drive;
    drive.max_accel = 50.0f;
    u->set_drive(drive);
    u->add_command_cap("RULEUCC_Patrol");
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

osc::sim::UnitCommand patrol(osc::f32 x, osc::f32 z, osc::u32 id) {
    osc::sim::UnitCommand c;
    c.type = CommandType::Patrol;
    c.target_pos = {x, 0.0f, z};
    c.command_id = id;
    return c;
}

void two_armies(SimState& sim) {
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.set_fog_of_war("none");
    sim.set_victory_condition("sandbox");
}

Unit* engineer(SimState& sim, osc::f32 x, osc::f32 z) {
    Unit* u = walker(sim, x, z);
    u->add_category("PATROLHELPER");
    u->add_category("RECLAIM");
    return u;
}

Unit* still(SimState& sim, int army, osc::f32 x, osc::f32 z) {
    auto u = std::make_unique<Unit>();
    u->set_army(army);
    u->set_position({x, 0.0f, z});
    u->set_max_health(100.0f);
    u->set_health(100.0f);
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

Prop* rock(SimState& sim, osc::f32 x, osc::f32 z, osc::f32 mass, osc::f32 energy,
           bool reclaimable = true) {
    auto p = std::make_unique<Prop>();
    p->set_position({x, 0.0f, z});
    p->reclaimable_category = reclaimable;
    p->reclaim_mass_max = mass;
    p->reclaim_energy_max = energy;
    auto* raw = p.get();
    sim.entity_registry().register_entity(std::move(p));
    return raw;
}

std::vector<osc::u32> break_offs(SimState& sim, Unit& u, CommandType type, int ticks) {
    std::vector<osc::u32> seen;
    bool was = false;
    for (int t = 0; t < ticks; ++t) {
        sim.tick();
        const auto& q = u.command_queue();
        const bool is = !q.empty() && q.front().from_patrol && q.front().type == type;
        if (is && !was) {
            seen.push_back(q.front().target_id);
        }
        was = is;
    }
    return seen;
}

void store(SimState& sim, double mass, double energy) {
    auto& economy = sim.army_at(0)->economy();
    economy.mass.max_storage = 1000.0;
    economy.mass.stored = mass;
    economy.energy.max_storage = 1000.0;
    economy.energy.stored = energy;
}

} // namespace

TEST_CASE("A patrol clicked once runs between the click and where it was given", "[patrol]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* u = walker(sim, 10.0f, 10.0f);
    osc::renderer::InputHandler input;
    input.set_player_army(0);
    input.set_selected({u->entity_id()});
    osc::renderer::CommandMode mode;
    mode.mode = "order";
    mode.name = "RULEUCC_Patrol";
    REQUIRE(input.click_in_command_mode(sim, mode, 60.0f, 10.0f, false));
    bool there = false;
    bool back = false;
    for (int t = 0; t < 600; ++t) {
        sim.tick();
        there |= u->position().x > 58.0f;
        back |= there && u->position().x < 12.0f;
    }
    CHECK(there);
    CHECK(back);
}

TEST_CASE("A patrol point added to a running patrol joins the loop after the last", "[patrol]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* u = walker(sim, 10.0f, 10.0f);
    u->push_command(patrol(60.0f, 10.0f, 1), true);
    u->push_command(patrol(60.0f, 60.0f, 2), false);
    for (int t = 0; t < 400 && u->command_queue().front().command_id != 2; ++t) {
        sim.tick();
    }
    REQUIRE(u->command_queue().front().command_id == 2);
    u->push_command(patrol(10.0f, 60.0f, 3), false);
    REQUIRE(u->command_queue().size() == 3);
    CHECK(u->command_queue()[1].command_id == 3);
    CHECK(u->command_queue()[2].command_id == 1);
}

TEST_CASE("A queued patrol starts where the orders before it end even before they run",
          "[patrol]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* u = walker(sim, 10.0f, 10.0f);
    osc::renderer::InputHandler input;
    input.set_player_army(0);
    input.set_selected({u->entity_id()});
    osc::renderer::CommandMode mode;
    mode.mode = "order";
    mode.name = "RULEUCC_Patrol";
    REQUIRE_FALSE(input.right_click_at(sim, 60.0f, 60.0f, false).empty());
    REQUIRE(input.click_in_command_mode(sim, mode, 90.0f, 60.0f, true));
    REQUIRE(input.click_in_command_mode(sim, mode, 90.0f, 90.0f, true));
    sim.tick();
    const auto& q = u->command_queue();
    REQUIRE(q.size() == 4);
    CHECK(q[0].type == CommandType::Move);
    CHECK(q[1].type == CommandType::Patrol);
    CHECK(q[1].target_pos.x == 60.0f);
    CHECK(q[1].target_pos.z == 60.0f);
    CHECK(q[3].target_pos.x == 90.0f);
    CHECK(q[3].target_pos.z == 90.0f);

    osc::sim::UnitCommand stop;
    stop.type = CommandType::Stop;
    sim.set_human_input_active(true);
    sim.route_player_command({u->entity_id()}, stop, true);
    sim.set_human_input_active(false);
    const osc::sim::Vector3 at = u->position();
    REQUIRE(input.click_in_command_mode(sim, mode, 30.0f, 10.0f, true));
    sim.tick();
    REQUIRE(q.size() == 2);
    const auto anchored = [&](const osc::sim::UnitCommand& c) {
        return c.target_pos.x == at.x && c.target_pos.z == at.z;
    };
    CHECK((anchored(q[0]) || anchored(q[1])));
}

TEST_CASE("A queued patrol starts where the last unit with orders ends them", "[patrol]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* busy = walker(sim, 10.0f, 10.0f);
    Unit* idle = walker(sim, 10.0f, 30.0f);
    osc::sim::UnitCommand move;
    move.type = CommandType::Move;
    move.target_pos = {60.0f, 0.0f, 60.0f};
    busy->push_command(move, true);
    osc::renderer::InputHandler input;
    input.set_player_army(0);
    input.set_selected({busy->entity_id(), idle->entity_id()});
    osc::renderer::CommandMode mode;
    mode.mode = "order";
    mode.name = "RULEUCC_Patrol";
    REQUIRE(input.click_in_command_mode(sim, mode, 90.0f, 10.0f, true));
    sim.tick();
    REQUIRE(idle->command_queue().size() == 2);
    CHECK(idle->command_queue()[0].target_pos.x == 60.0f);
    CHECK(idle->command_queue()[0].target_pos.z == 60.0f);
}

TEST_CASE("A patrol breaks off to attack an enemy within its GuardScanRadius of the route",
          "[patrol]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* tank = walker(sim, 10.0f, 10.0f);
    auto gun = std::make_unique<osc::sim::Weapon>();
    gun->max_range = 5.0f;
    tank->add_weapon(std::move(gun));
    tank->set_guard_scan_radius(20.0f);
    Unit* near = still(sim, 1, 40.0f, 25.0f);
    still(sim, 1, 70.0f, 40.0f);
    tank->push_command(patrol(100.0f, 10.0f, 1), true);
    const auto first = break_offs(sim, *tank, CommandType::Attack, 80);
    REQUIRE(first == std::vector<osc::u32>{near->entity_id()});
    sim.entity_registry().unregister_entity(near->entity_id());
    const auto rest = break_offs(sim, *tank, CommandType::Attack, 300);
    CHECK(rest.empty());
    CHECK(tank->position().x > 95.0f);
}

TEST_CASE("A patrol goes for an enemy ahead on its route only within its GuardScanRadius",
          "[patrol]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* tank = walker(sim, 10.0f, 10.0f);
    auto gun = std::make_unique<osc::sim::Weapon>();
    gun->max_range = 5.0f;
    tank->add_weapon(std::move(gun));
    still(sim, 1, 90.0f, 10.0f);
    tank->push_command(patrol(100.0f, 10.0f, 1), true);
    for (int t = 0; t < 300 && !tank->command_queue().front().from_patrol; ++t) {
        sim.tick();
    }
    REQUIRE(tank->command_queue().front().type == CommandType::Attack);
    CHECK(tank->position().x >= 65.0f);
}

TEST_CASE("A patrolling engineer reclaims a RECLAIMABLE prop on its route once a leg", "[patrol]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* eng = engineer(sim, 10.0f, 10.0f);
    rock(sim, 30.0f, 12.0f, 1.0f, 0.0f, false);
    Prop* stone = rock(sim, 50.0f, 14.0f, 1.0f, 0.0f);
    eng->push_command(patrol(90.0f, 10.0f, 1), true);
    const auto seen = break_offs(sim, *eng, CommandType::Reclaim, 400);
    CHECK(seen == std::vector<osc::u32>{stone->entity_id()});
}

TEST_CASE("A patrolling engineer reclaims a still enemy at half its distance's weight",
          "[patrol]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* eng = engineer(sim, 10.0f, 10.0f);
    rock(sim, 30.0f, 10.0f, 1.0f, 0.0f);
    Unit* enemy = still(sim, 1, 36.0f, 10.0f);
    enemy->add_category("RECLAIMABLE");
    eng->push_command(patrol(90.0f, 10.0f, 1), true);
    const auto seen = break_offs(sim, *eng, CommandType::Reclaim, 3);
    CHECK(seen == std::vector<osc::u32>{enemy->entity_id()});
}

TEST_CASE("A patrolling engineer repairs a damaged ally only with both stores nearly full",
          "[patrol]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* eng = engineer(sim, 10.0f, 10.0f);
    Unit* ally = still(sim, 0, 30.0f, 14.0f);
    ally->set_health(50.0f);
    eng->push_command(patrol(90.0f, 10.0f, 1), true);
    store(sim, 900.0, 100.0);
    CHECK(break_offs(sim, *eng, CommandType::Repair, 12).empty());
    store(sim, 900.0, 900.0);
    CHECK(break_offs(sim, *eng, CommandType::Repair, 12) ==
          std::vector<osc::u32>{ally->entity_id()});
}

TEST_CASE("A patrolling engineer leaves a prop of a resource it has no room for", "[patrol]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* eng = engineer(sim, 10.0f, 10.0f);
    rock(sim, 25.0f, 10.0f, 1.0f, 0.0f);
    Prop* tree = rock(sim, 40.0f, 10.0f, 0.0f, 1.0f);
    eng->push_command(patrol(90.0f, 10.0f, 1), true);
    store(sim, 900.0, 100.0);
    CHECK(break_offs(sim, *eng, CommandType::Reclaim, 3) ==
          std::vector<osc::u32>{tree->entity_id()});
}

TEST_CASE("A patrol point added while the patrol has broken off joins the loop", "[patrol]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* eng = engineer(sim, 10.0f, 10.0f);
    eng->push_command(patrol(60.0f, 10.0f, 1), true);
    eng->push_command(patrol(60.0f, 60.0f, 2), false);
    for (int t = 0; t < 400 && eng->command_queue().front().command_id != 2; ++t) {
        sim.tick();
    }
    REQUIRE(eng->command_queue().front().command_id == 2);
    rock(sim, 62.0f, 40.0f, 1.0f, 0.0f);
    for (int t = 0; t < 12 && !eng->command_queue().front().from_patrol; ++t) {
        sim.tick();
    }
    REQUIRE(eng->command_queue().front().from_patrol);
    eng->push_command(patrol(10.0f, 60.0f, 3), false);
    const auto& q = eng->command_queue();
    REQUIRE(q.size() == 4);
    CHECK(q[1].command_id == 2);
    CHECK(q[2].command_id == 3);
    CHECK(q[3].command_id == 1);
}

TEST_CASE("A unit that broke off its patrol is still Patrolling", "[patrol][lua]") {
    osc::lua::LuaState lua;
    SimState sim(lua.raw(), nullptr);
    osc::lua::register_moho_bindings(lua, sim);
    Unit unit;
    osc::sim::UnitCommand reclaim;
    reclaim.type = CommandType::Reclaim;
    reclaim.from_patrol = true;
    unit.push_command(reclaim, true);
    unit.push_command(patrol(60.0f, 10.0f, 1), false);
    lua_State* L = lua.raw();
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, &unit);
    lua_rawset(L, -3);
    lua_setglobal(L, "unit");
    const auto r = lua.do_string(R"(
        if not moho.unit_methods.IsUnitState(unit, 'Patrolling') then error('not patrolling') end
    )");
    if (!r) {
        FAIL(r.error().message);
    }
}

TEST_CASE("A patrol broken off to reclaim plays on the same after a restore",
          "[patrol][savegame][sync]") {
    const auto world = [](SimState& sim, lua_State* L) {
        flat(sim);
        two_armies(sim);
        lua_newtable(L);
        lua_pushstring(L, "/rock_prop.bp");
        lua_newtable(L);
        lua_pushstring(L, "Categories");
        lua_newtable(L);
        lua_pushstring(L, "RECLAIMABLE");
        lua_rawseti(L, -2, 1);
        lua_rawset(L, -3);
        lua_pushstring(L, "Economy");
        lua_newtable(L);
        lua_pushstring(L, "ReclaimMassMax");
        lua_pushnumber(L, 1);
        lua_rawset(L, -3);
        lua_rawset(L, -3);
        lua_rawset(L, -3);
        lua_setglobal(L, "__blueprints");
        Unit* eng = engineer(sim, 10.0f, 10.0f);
        for (const osc::f32 x : {30.0f, 50.0f, 70.0f}) {
            Prop* p = rock(sim, x, 14.0f, 0.0f, 0.0f, false);
            p->set_blueprint_id("/rock_prop.bp");
            osc::sim::read_prop_blueprint(L, *p);
        }
        osc::sim::GameSetup game;
        game.scenario = "/maps/test/test_scenario.lua";
        game.seed = 31;
        sim.set_game_setup(game);
        eng->push_command(patrol(90.0f, 10.0f, 1), true);
        eng->push_command(patrol(10.0f, 10.0f, 2), false);
        return eng->entity_id();
    };
    LuaGuard ga;
    SimState a(ga.L, nullptr);
    a.set_seed(31);
    const osc::u32 id = world(a, ga.L);
    a.set_recording(true);
    const auto* eng = static_cast<const Unit*>(a.entity_registry().find(id));
    for (int t = 0; t < 400 && eng->command_queue()[1].patrol_claimed.size() < 2; ++t) {
        a.tick();
    }
    REQUIRE(eng->command_queue()[1].patrol_claimed.size() == 2);
    const osc::sim::SavedGame save = osc::sim::save_game(a, "patrolling");
    for (int t = 0; t < 300; ++t) {
        a.tick();
    }

    LuaGuard gb;
    SimState b(gb.L, nullptr);
    b.set_seed(31);
    REQUIRE(world(b, gb.L) == id);
    const std::string err = osc::sim::load_snapshot(b, save.snapshot);
    INFO(err);
    REQUIRE(err.empty());
    b.adopt_history(save.game);
    for (int t = 0; t < 300; ++t) {
        b.tick();
    }
    CHECK(b.compute_sync_checksum() == a.compute_sync_checksum());
}
