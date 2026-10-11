#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
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
#include <lauxlib.h>
#include <lua.h>
}

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
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

TEST_CASE("A NeedUnpack patrol breaks off only for an enemy it can hit from where it stands",
          "[patrol]") {
    // FindBestEnemy: a NeedUnpack unit's candidate needs an Available firing
    // solution, so its patrol leaves alone what is within GuardScanRadius but
    // out of its weapon's reach.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* tank = walker(sim, 10.0f, 10.0f);
    auto gun = std::make_unique<osc::sim::Weapon>();
    gun->max_range = 5.0f;
    tank->add_weapon(std::move(gun));
    tank->set_guard_scan_radius(20.0f);
    tank->set_need_unpack(true);
    still(sim, 1, 40.0f, 25.0f);
    Unit* on_route = still(sim, 1, 60.0f, 12.0f);
    tank->push_command(patrol(100.0f, 10.0f, 1), true);
    const auto seen = break_offs(sim, *tank, CommandType::Attack, 300);
    REQUIRE(!seen.empty());
    CHECK(seen.front() == on_route->entity_id());
}

TEST_CASE("A patrol's chase ends at GuardReturnRadius from where it broke off", "[patrol]") {
    // Moho's patrol task keeps where it broke off (GuardedPos), and the
    // acquire task's leash ends the attack once the unit, having reached its
    // target, is GuardReturnRadius from there.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* tank = walker(sim, 10.0f, 10.0f);
    auto gun = std::make_unique<osc::sim::Weapon>();
    gun->max_range = 5.0f;
    tank->add_weapon(std::move(gun));
    tank->set_guard_scan_radius(20.0f);
    tank->set_guard_return_radius(30.0f);
    Unit* runner = walker(sim, 22.0f, 18.0f);
    runner->set_army(1);
    runner->set_max_speed(3.0f);
    osc::sim::UnitCommand away;
    away.type = CommandType::Move;
    away.target_pos = {22.0f, 0.0f, 120.0f};
    away.command_id = 9;
    runner->push_command(away, true);
    tank->push_command(patrol(100.0f, 10.0f, 1), true);
    const auto fighting = [&] {
        const auto& q = tank->command_queue();
        return !q.empty() && q.front().from_patrol && q.front().type == CommandType::Attack;
    };
    for (int t = 0; t < 30 && !fighting(); ++t) sim.tick();
    REQUIRE(fighting());
    const osc::sim::Vector3 from = tank->command_queue().front().leash_anchor_pos;
    CHECK(std::hypot(from.x - tank->position().x, from.z - tank->position().z) < 1.0f);
    float farthest = 0;
    bool armed = false;
    for (int t = 0; t < 400 && fighting(); ++t) {
        sim.tick();
        farthest = std::max(farthest, static_cast<float>(std::hypot(tank->position().x - from.x,
                                                                    tank->position().z - from.z)));
        armed = armed || (fighting() && tank->command_queue().front().leash_armed);
    }
    CHECK(armed);
    CHECK_FALSE(fighting());
    CHECK(farthest <= 32.0f);
    REQUIRE(!tank->command_queue().empty());
    CHECK(tank->command_queue().front().type == CommandType::Patrol);
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
    const osc::u32 stone = rock(sim, 50.0f, 14.0f, 1.0f, 0.0f)->entity_id();
    eng->push_command(patrol(90.0f, 10.0f, 1), true);
    const auto seen = break_offs(sim, *eng, CommandType::Reclaim, 400);
    CHECK(seen == std::vector<osc::u32>{stone});
}

TEST_CASE("A patrol back from reclaiming looks for the next prop at once", "[patrol]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* eng = engineer(sim, 10.0f, 10.0f);
    rock(sim, 30.0f, 14.0f, 1.0f, 0.0f);
    rock(sim, 24.0f, 14.0f, 1.0f, 0.0f);
    eng->push_command(patrol(90.0f, 10.0f, 1), true);
    const auto patrolling = [&] { return !eng->command_queue().front().from_patrol; };
    int t = 0;
    for (; t < 400 && patrolling(); ++t) {
        sim.tick();
    }
    REQUIRE_FALSE(patrolling());
    for (; t < 400 && !patrolling(); ++t) {
        sim.tick();
    }
    const auto claimed = [&] { return eng->command_queue().back().patrol_claimed.size(); };
    int heading_on = 0;
    for (; t < 400 && patrolling() && claimed() < 2; ++t) {
        sim.tick();
        ++heading_on;
    }
    CHECK(claimed() == 2);
    CHECK(heading_on == 0);
}

TEST_CASE("A patrol leg looks over all of itself, behind the unit too", "[patrol]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* eng = engineer(sim, 10.0f, 10.0f);
    eng->push_command(patrol(110.0f, 10.0f, 1), true);
    for (int t = 0; t < 400 && eng->position().x < 70.0f; ++t) {
        sim.tick();
    }
    REQUIRE(eng->position().x >= 70.0f);
    Prop* behind = rock(sim, 12.0f, 14.0f, 1.0f, 0.0f);
    CHECK(break_offs(sim, *eng, CommandType::Reclaim, 7) ==
          std::vector<osc::u32>{behind->entity_id()});
}

TEST_CASE("A patrol's break-off starts 6 ticks after the patrol looked", "[patrol]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* eng = engineer(sim, 10.0f, 10.0f);
    rock(sim, 30.0f, 14.0f, 1.0f, 0.0f);
    eng->push_command(patrol(90.0f, 10.0f, 1), true);
    for (int t = 0; t < 12 && !eng->command_queue().front().from_patrol; ++t) {
        sim.tick();
    }
    REQUIRE(eng->command_queue().front().from_patrol);
    std::vector<bool> walking;
    for (int t = 0; t < 6; ++t) {
        sim.tick();
        walking.push_back(eng->command_queue().front().approached);
    }
    CHECK(walking == std::vector<bool>{false, false, false, false, false, true});
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

TEST_CASE("An attack-move goes for an enemy near its route, then ends where it arrives",
          "[patrol][attack-move]") {
    // Moho's AggressiveMove: one leg of its patrol task, the order removed
    // when the leg is done.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* tank = walker(sim, 10.0f, 10.0f);
    auto gun = std::make_unique<osc::sim::Weapon>();
    gun->max_range = 5.0f;
    tank->add_weapon(std::move(gun));
    tank->set_guard_scan_radius(20.0f);
    Unit* near = still(sim, 1, 50.0f, 22.0f);
    osc::sim::UnitCommand go;
    go.type = CommandType::AggressiveMove;
    go.target_pos = {100.0f, 0.0f, 10.0f};
    go.command_id = 1;
    tank->push_command(go, true);
    const auto seen = break_offs(sim, *tank, CommandType::Attack, 120);
    REQUIRE(seen == std::vector<osc::u32>{near->entity_id()});
    sim.entity_registry().unregister_entity(near->entity_id());
    for (int t = 0; t < 400 && !tank->command_queue().empty(); ++t) {
        sim.tick();
    }
    CHECK(tank->command_queue().empty());
    CHECK(tank->position().x > 95.0f);
}

TEST_CASE("A commander on attack-move leaves the reclaiming to others; on patrol it reclaims",
          "[patrol][attack-move]") {
    // Moho's patrol task in formation, as an attack-move's always is: COMMAND
    // and SUBCOMMANDER units skip the helpers' sweep.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* acu = engineer(sim, 10.0f, 10.0f);
    acu->add_category("COMMAND");
    const osc::u32 stone = rock(sim, 50.0f, 14.0f, 1.0f, 0.0f)->entity_id();
    osc::sim::UnitCommand go;
    go.type = CommandType::AggressiveMove;
    go.target_pos = {90.0f, 0.0f, 10.0f};
    go.command_id = 1;
    acu->push_command(go, true);
    CHECK(break_offs(sim, *acu, CommandType::Reclaim, 300).empty());

    acu->push_command(patrol(10.0f, 10.0f, 2), true);
    CHECK(break_offs(sim, *acu, CommandType::Reclaim, 300) == std::vector<osc::u32>{stone});
}

TEST_CASE("A support commander on attack-move leaves the reclaiming to others, as retail's "
          "SUBCOMMANDER or FAF's SACU_BEHAVIOR",
          "[patrol][attack-move]") {
    for (const char* category : {"SUBCOMMANDER", "SACU_BEHAVIOR"}) {
        CAPTURE(category);
        LuaGuard g;
        SimState sim(g.L, nullptr);
        flat(sim);
        two_armies(sim);
        Unit* sacu = engineer(sim, 10.0f, 10.0f);
        sacu->add_category(category);
        rock(sim, 50.0f, 14.0f, 1.0f, 0.0f);
        osc::sim::UnitCommand go;
        go.type = CommandType::AggressiveMove;
        go.target_pos = {90.0f, 0.0f, 10.0f};
        go.command_id = 1;
        sacu->push_command(go, true);
        CHECK(break_offs(sim, *sacu, CommandType::Reclaim, 300).empty());
    }
}

TEST_CASE("An Attack on bare ground attack-moves a mobile unit on ReturnFire, and not others",
          "[patrol][attack-move]") {
    // Moho's SplitSelectionForAggressiveMove (the UI's RULEUCC_Attack arm).
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* eager = walker(sim, 10.0f, 10.0f);
    Unit* holding = walker(sim, 10.0f, 30.0f);
    for (Unit* u : {eager, holding}) {
        u->add_command_cap("RULEUCC_Attack");
        u->set_motion_type("RULEUMT_Land"); // mobile
    }
    holding->set_fire_state(1); // HoldFire
    osc::renderer::InputHandler input;
    input.set_player_army(0);
    input.set_selected({eager->entity_id(), holding->entity_id()});
    osc::renderer::CommandMode mode;
    mode.mode = "order";
    mode.name = "RULEUCC_Attack";
    REQUIRE(input.click_in_command_mode(sim, mode, 90.0f, 60.0f, false));
    sim.tick();
    REQUIRE(!eager->command_queue().empty());
    CHECK(eager->command_queue().front().type == CommandType::AggressiveMove);
    CHECK(eager->command_queue().front().target_pos.x == 90.0f);
    for (const auto& c : holding->command_queue()) {
        CHECK(c.type != CommandType::AggressiveMove);
    }
}

TEST_CASE("Reclaim takes only what Moho lets it reclaim", "[reclaim]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* eng = engineer(sim, 10.0f, 10.0f);
    const auto takes = [&](const osc::sim::Entity& target) {
        osc::sim::UnitCommand reclaim;
        reclaim.type = CommandType::Reclaim;
        reclaim.target_id = target.entity_id();
        return sim.route_command({eng->entity_id()}, reclaim, true) != 0;
    };
    Unit* acu = still(sim, 1, 20.0f, 10.0f);
    CHECK_FALSE(takes(*acu));
    Unit* tank = still(sim, 1, 20.0f, 20.0f);
    tank->add_category("RECLAIMABLE");
    CHECK(takes(*tank));
    tank->set_reclaimable(false);
    CHECK_FALSE(takes(*tank));
    Unit* plane = still(sim, 1, 20.0f, 30.0f);
    plane->add_category("RECLAIMABLE");
    plane->set_layer("Air");
    CHECK_FALSE(takes(*plane));
    Unit* frame = still(sim, 1, 20.0f, 40.0f);
    frame->set_is_being_built(true);
    CHECK(takes(*frame));
    CHECK_FALSE(takes(*rock(sim, 30.0f, 10.0f, 5.0f, 0.0f, false)));
    CHECK(takes(*rock(sim, 30.0f, 20.0f, 5.0f, 0.0f)));
}

TEST_CASE("A queued Reclaim of what is not RECLAIMABLE is dropped", "[reclaim]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* eng = engineer(sim, 10.0f, 10.0f);
    eng->set_build_rate(10.0f);
    eng->set_max_build_distance(5.0f);
    Prop* bridge = rock(sim, 12.0f, 10.0f, 5.0f, 0.0f, false);
    lua_newtable(g.L);
    lua_pushstring(g.L, "MaxMassReclaim");
    lua_pushnumber(g.L, 500);
    lua_rawset(g.L, -3);
    bridge->set_lua_table_ref(luaL_ref(g.L, LUA_REGISTRYINDEX));
    osc::sim::UnitCommand reclaim;
    reclaim.type = CommandType::Reclaim;
    reclaim.target_id = bridge->entity_id();
    eng->push_command(reclaim, true);
    sim.tick();
    CHECK(eng->command_queue().empty());
    CHECK_FALSE(eng->is_reclaiming());
    CHECK(bridge->fraction_complete() == 1.0f);
}

TEST_CASE("The reclaim cursor and right-click pass over what is not RECLAIMABLE", "[reclaim]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* eng = engineer(sim, 10.0f, 10.0f);
    eng->add_command_cap("RULEUCC_Reclaim");
    eng->add_command_cap("RULEUCC_Move");
    rock(sim, 40.0f, 10.0f, 5.0f, 0.0f, false);
    still(sim, 1, 40.0f, 40.0f);
    osc::renderer::InputHandler input;
    input.set_player_army(0);
    input.set_selected({eng->entity_id()});
    osc::renderer::CommandMode mode;
    mode.mode = "order";
    mode.name = "RULEUCC_Reclaim";
    CHECK_FALSE(input.click_in_command_mode(sim, mode, 40.0f, 10.0f, false));
    CHECK_FALSE(input.click_in_command_mode(sim, mode, 40.0f, 40.0f, false));
    const auto orders = input.right_click_orders(sim, 40.0f, 10.0f);
    REQUIRE(orders.size() == 1);
    CHECK(orders.front().first.type == CommandType::Move);
    rock(sim, 60.0f, 10.0f, 5.0f, 0.0f);
    CHECK(input.click_in_command_mode(sim, mode, 60.0f, 10.0f, false));
}

TEST_CASE("An upgrade's unfinished unit is hovered as the structure upgrading", "[hover]") {
    LuaGuard g;
    osc::blueprints::BlueprintStore store(g.L);
    for (const char* bp : {"return {BlueprintId = 'tier1', General = {UpgradesFrom = 'none'}}",
                           "return {BlueprintId = 'tier2', General = {UpgradesFrom = 'tier1'}}"}) {
        REQUIRE(luaL_loadbuffer(g.L, bp, std::strlen(bp), "bp") == 0);
        REQUIRE(lua_pcall(g.L, 0, 1, 0) == 0);
        store.register_blueprint(g.L, osc::blueprints::BlueprintType::Unit, lua_gettop(g.L));
        lua_pop(g.L, 1);
    }
    SimState sim(g.L, &store);
    flat(sim);
    two_armies(sim);
    Unit* eng = engineer(sim, 10.0f, 10.0f);
    eng->add_command_cap("RULEUCC_Repair");
    eng->add_command_cap("RULEUCC_Guard");
    eng->add_command_cap("RULEUCC_Move");
    Unit* old = still(sim, 0, 40.0f, 40.0f);
    old->set_blueprint_id("tier1");
    old->set_size_xz(4.0f, 4.0f);
    old->set_size_y(1.0f);
    Unit* next = still(sim, 0, 40.0f, 40.0f);
    next->set_blueprint_id("tier2");
    next->set_size_xz(5.0f, 5.0f);
    next->set_size_y(3.0f);
    next->set_is_being_built(true);
    next->set_creator_id(old->entity_id());
    osc::renderer::InputHandler input;
    input.set_player_army(0);
    input.set_selected({eng->entity_id()});
    const auto orders = input.right_click_orders(sim, 40.0f, 40.0f);
    REQUIRE(orders.size() == 1);
    CHECK(orders.front().first.type == CommandType::Guard);
    CHECK(orders.front().first.target_id == old->entity_id());
}

TEST_CASE("Past zoom 150 the cursor passes over props", "[reclaim]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* eng = engineer(sim, 10.0f, 10.0f);
    eng->add_command_cap("RULEUCC_Reclaim");
    eng->add_command_cap("RULEUCC_Move");
    rock(sim, 40.0f, 10.0f, 5.0f, 0.0f);
    osc::renderer::InputHandler input;
    input.set_player_army(0);
    input.set_selected({eng->entity_id()});
    input.set_camera_zoom(150.0f);
    CHECK(input.right_button_order(sim, 40.0f, 10.0f) == CommandType::Reclaim);
    input.set_camera_zoom(151.0f);
    CHECK(input.right_button_order(sim, 40.0f, 10.0f) == CommandType::Move);
    osc::renderer::CommandMode mode;
    mode.mode = "order";
    mode.name = "RULEUCC_Reclaim";
    CHECK_FALSE(input.click_in_command_mode(sim, mode, 40.0f, 10.0f, false));
}

TEST_CASE("Capture targets the unit under the cursor, not one beside it", "[capture]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* eng = engineer(sim, 10.0f, 10.0f);
    eng->add_command_cap("RULEUCC_Capture");
    eng->add_command_cap("RULEUCC_Move");
    Unit* enemy = still(sim, 1, 40.0f, 10.0f);
    osc::renderer::InputHandler input;
    input.set_player_army(0);
    input.set_selected({eng->entity_id()});
    osc::renderer::CommandMode mode;
    mode.mode = "order";
    mode.name = "RULEUCC_Capture";
    CHECK(input.right_button_order(sim, 40.3f, 10.0f) == CommandType::Capture);
    CHECK(input.right_button_order(sim, 43.0f, 10.0f) == CommandType::Move);
    CHECK_FALSE(input.click_in_command_mode(sim, mode, 43.0f, 10.0f, false));
    const auto issued = input.click_in_command_mode(sim, mode, 40.3f, 10.0f, false);
    REQUIRE(issued);
    CHECK(issued->target_id == enemy->entity_id());
}

TEST_CASE("Capture is offered only on a capturable unit that rides nothing", "[capture]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* eng = engineer(sim, 10.0f, 10.0f);
    eng->add_command_cap("RULEUCC_Capture");
    eng->add_command_cap("RULEUCC_Move");
    still(sim, 1, 40.0f, 10.0f)->set_capturable(false);
    Unit* carried = still(sim, 1, 40.0f, 40.0f);
    carried->set_parent(still(sim, 1, 70.0f, 70.0f)->entity_id(), 0);
    Unit* scrap = still(sim, 1, 70.0f, 10.0f);
    scrap->set_capturable(false);
    scrap->add_category("RECLAIMABLE");
    osc::renderer::InputHandler input;
    input.set_player_army(0);
    input.set_selected({eng->entity_id()});
    osc::renderer::CommandMode mode;
    mode.mode = "order";
    mode.name = "RULEUCC_Capture";
    CHECK(input.right_button_order(sim, 40.0f, 10.0f) == CommandType::Move);
    CHECK(input.right_button_order(sim, 40.0f, 40.0f) == CommandType::Move);
    CHECK_FALSE(input.click_in_command_mode(sim, mode, 40.0f, 10.0f, false));
    CHECK_FALSE(input.click_in_command_mode(sim, mode, 40.0f, 40.0f, false));
    eng->add_command_cap("RULEUCC_Reclaim");
    CHECK(input.right_button_order(sim, 70.0f, 10.0f) == CommandType::Reclaim);
}

TEST_CASE("A right click attacks only an enemy a selected weapon can hit", "[attack]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* tank = walker(sim, 10.0f, 10.0f);
    tank->set_motion_type("RULEUMT_Land");
    tank->add_command_cap("RULEUCC_Attack");
    tank->add_command_cap("RULEUCC_Move");
    auto gun = std::make_unique<osc::sim::Weapon>();
    gun->max_range = 20.0f;
    gun->fire_target_layer_caps = osc::sim::parse_layer_caps("Land|Water");
    tank->add_weapon(std::move(gun));
    Unit* enemy = still(sim, 1, 40.0f, 10.0f);
    enemy->add_category("RECLAIMABLE");
    Unit* bomber = still(sim, 1, 40.0f, 40.0f);
    bomber->set_layer("Air");
    osc::renderer::InputHandler input;
    input.set_player_army(0);
    input.set_selected({tank->entity_id()});
    CHECK(input.right_button_order(sim, 40.0f, 10.0f) == CommandType::Attack);
    CHECK(input.right_button_order(sim, 43.0f, 10.0f) == CommandType::Move);
    const auto issued = input.right_click_at(sim, 40.0f, 10.0f, false);
    REQUIRE(issued.size() == 1);
    CHECK(issued.front().type == "Attack");
    CHECK(issued.front().target_id == enemy->entity_id());

    Unit* eng = engineer(sim, 10.0f, 20.0f);
    eng->add_command_cap("RULEUCC_Capture");
    eng->add_command_cap("RULEUCC_Reclaim");
    eng->add_command_cap("RULEUCC_Move");
    input.set_selected({eng->entity_id()});
    CHECK(input.right_button_order(sim, 40.0f, 10.0f) == CommandType::Capture);
    input.set_selected({tank->entity_id()});
    CHECK_FALSE(input.right_button_order(sim, 40.0f, 40.0f));
    CHECK(input.right_click_invalid(sim, 40.0f, 40.0f));
    CHECK(input.right_click_at(sim, 40.0f, 40.0f, false).empty());

    Unit* flak = walker(sim, 12.0f, 10.0f);
    flak->set_motion_type("RULEUMT_Land");
    flak->add_command_cap("RULEUCC_Attack");
    auto aa = std::make_unique<osc::sim::Weapon>();
    aa->fire_target_layer_caps = osc::sim::parse_layer_caps("Air");
    flak->add_weapon(std::move(aa));
    input.set_selected({tank->entity_id(), flak->entity_id()});
    CHECK(input.right_button_order(sim, 40.0f, 40.0f) == CommandType::Attack);
    CHECK_FALSE(input.right_click_invalid(sim, 40.0f, 40.0f));
}

TEST_CASE("A right click gives the whole selection one order", "[attack][capture]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* tank = walker(sim, 10.0f, 10.0f);
    tank->set_motion_type("RULEUMT_Land");
    tank->add_command_cap("RULEUCC_Attack");
    tank->add_command_cap("RULEUCC_Move");
    auto gun = std::make_unique<osc::sim::Weapon>();
    gun->max_range = 20.0f;
    gun->fire_target_layer_caps = osc::sim::parse_layer_caps("Land|Water");
    tank->add_weapon(std::move(gun));
    Unit* eng = engineer(sim, 12.0f, 10.0f);
    eng->set_motion_type("RULEUMT_Land");
    eng->add_command_cap("RULEUCC_Move");
    eng->add_command_cap("RULEUCC_Capture");
    eng->add_command_cap("RULEUCC_Reclaim");
    Unit* flak = walker(sim, 14.0f, 10.0f);
    flak->set_motion_type("RULEUMT_Land");
    flak->add_command_cap("RULEUCC_Attack");
    auto aa = std::make_unique<osc::sim::Weapon>();
    aa->fire_target_layer_caps = osc::sim::parse_layer_caps("Air");
    flak->add_weapon(std::move(aa));
    const Unit* enemy = still(sim, 1, 40.0f, 10.0f);
    rock(sim, 70.0f, 40.0f, 10.0f, 0.0f);
    osc::renderer::InputHandler input;
    input.set_player_army(0);
    const auto one_order = [&](osc::f32 x, osc::f32 z) {
        const auto orders = input.right_click_orders(sim, x, z);
        REQUIRE(orders.size() == 1);
        return orders.front();
    };

    input.set_selected({tank->entity_id(), eng->entity_id()});
    auto order = one_order(40.0f, 10.0f);
    CHECK(order.first.type == CommandType::Attack);
    CHECK(order.first.target_id == enemy->entity_id());
    CHECK(order.second == std::vector<osc::u32>{tank->entity_id(), eng->entity_id()});
    order = one_order(70.0f, 40.0f);
    CHECK(order.first.type == CommandType::Reclaim);
    CHECK(order.second.size() == 2);

    input.set_selected({eng->entity_id(), flak->entity_id()});
    order = one_order(40.0f, 10.0f);
    CHECK(order.first.type == CommandType::Capture);
    CHECK(order.second == std::vector<osc::u32>{eng->entity_id(), flak->entity_id()});
}

TEST_CASE("A right click on an own unit gives the whole selection one order",
          "[guard][transport]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    const auto mobile = [&](osc::f32 x, osc::f32 z, const char* motion) {
        Unit* u = walker(sim, x, z);
        u->set_motion_type(motion);
        for (const char* cap : {"RULEUCC_Move", "RULEUCC_Guard", "RULEUCC_CallTransport"}) {
            u->add_command_cap(cap);
        }
        return u;
    };
    Unit* eng = mobile(10.0f, 10.0f, "RULEUMT_Land");
    eng->add_command_cap("RULEUCC_Repair");
    Unit* tank = mobile(12.0f, 10.0f, "RULEUMT_Land");
    Unit* fighter = mobile(14.0f, 10.0f, "RULEUMT_Air");
    Unit* frame = still(sim, 0, 40.0f, 10.0f);
    frame->set_is_being_built(true);
    Unit* factory = still(sim, 0, 40.0f, 40.0f);
    factory->add_category("FACTORY");
    factory->set_is_being_built(true);
    Unit* damaged = still(sim, 0, 70.0f, 10.0f);
    damaged->set_health(50.0f);
    Unit* transport = mobile(70.0f, 40.0f, "RULEUMT_Air");
    transport->add_category("TRANSPORTATION");
    Unit* pad = still(sim, 0, 100.0f, 10.0f);
    pad->add_category("AIRSTAGINGPLATFORM");
    osc::renderer::InputHandler input;
    input.set_player_army(0);
    const auto one_order = [&](const Unit& on) {
        const auto orders = input.right_click_orders(sim, on.position().x, on.position().z);
        REQUIRE(orders.size() == 1);
        CHECK(orders.front().first.target_id == on.entity_id());
        return orders.front();
    };

    input.set_selected({eng->entity_id(), tank->entity_id()});
    auto order = one_order(*frame);
    CHECK(order.first.type == CommandType::Repair);
    CHECK(order.second == std::vector<osc::u32>{eng->entity_id(), tank->entity_id()});
    CHECK(one_order(*factory).first.type == CommandType::Guard);
    CHECK(one_order(*damaged).first.type == CommandType::Guard);

    input.set_selected({tank->entity_id(), fighter->entity_id()});
    order = one_order(*transport);
    CHECK(order.first.type == CommandType::TransportLoad);
    CHECK(order.second == std::vector<osc::u32>{tank->entity_id(), fighter->entity_id()});
    order = one_order(*pad);
    CHECK(order.first.type == CommandType::TransportLoad);
    CHECK(order.second == std::vector<osc::u32>{tank->entity_id(), fighter->entity_id()});

    input.set_selected({tank->entity_id()});
    const auto orders = input.right_click_orders(sim, tank->position().x, tank->position().z);
    REQUIRE(orders.size() == 1);
    CHECK(orders.front().first.type == CommandType::Move);
    CHECK(orders.front().second == std::vector<osc::u32>{tank->entity_id()});
}

TEST_CASE("A load order goes only to a unit its transport can carry", "[transport]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    two_armies(sim);
    const auto mobile = [&](osc::f32 x, const char* motion) {
        Unit* u = walker(sim, x, 10.0f);
        u->set_motion_type(motion);
        u->add_command_cap("RULEUCC_CallTransport");
        return u;
    };
    Unit* tank = mobile(10.0f, "RULEUMT_Land");
    Unit* fighter = mobile(12.0f, "RULEUMT_Air");
    fighter->add_category("AIR");
    Unit* acu = mobile(14.0f, "RULEUMT_Land");
    acu->add_category("COMMAND");
    Unit* walker_only = walker(sim, 16.0f, 10.0f);
    walker_only->set_motion_type("RULEUMT_Land");
    Unit* transport = mobile(40.0f, "RULEUMT_Air");
    transport->add_category("AIR");
    transport->add_category("TRANSPORTATION");
    transport->add_command_cap("RULEUCC_Transport");
    Unit* pad = still(sim, 0, 60.0f, 10.0f);
    pad->add_category("AIRSTAGINGPLATFORM");
    pad->add_command_cap("RULEUCC_Transport");
    Unit* beacon = still(sim, 0, 80.0f, 10.0f);
    beacon->add_category("FERRYBEACON");
    const Unit* other = still(sim, 0, 90.0f, 10.0f);
    const auto takes = [&](const Unit& u, CommandType type, const Unit& onto) {
        osc::sim::UnitCommand load;
        load.type = type;
        load.target_id = onto.entity_id();
        return sim.takes_command(u, load);
    };
    CHECK(takes(*tank, CommandType::TransportLoad, *transport));
    CHECK(takes(*transport, CommandType::TransportLoad, *transport));
    CHECK_FALSE(takes(*fighter, CommandType::TransportLoad, *transport));
    CHECK_FALSE(takes(*walker_only, CommandType::TransportLoad, *transport));
    CHECK_FALSE(takes(*acu, CommandType::TransportLoad, *transport));
    transport->add_category("CANTRANSPORTCOMMANDER");
    CHECK(takes(*acu, CommandType::TransportLoad, *transport));
    CHECK(takes(*fighter, CommandType::TransportLoad, *pad));
    CHECK(takes(*fighter, CommandType::Dock, *pad));
    CHECK_FALSE(takes(*tank, CommandType::TransportLoad, *pad));
    CHECK_FALSE(takes(*tank, CommandType::TransportLoad, *other));
    CHECK(takes(*tank, CommandType::WaitForFerry, *beacon));
    CHECK_FALSE(takes(*transport, CommandType::WaitForFerry, *beacon));
}

TEST_CASE("An attack order goes to no unarmed structure and onto no ally", "[attack]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    two_armies(sim);
    Unit* factory = still(sim, 0, 10.0f, 10.0f);
    Unit* tank = walker(sim, 12.0f, 10.0f);
    tank->set_motion_type("RULEUMT_Land");
    const Unit* enemy = still(sim, 1, 40.0f, 10.0f);
    const Unit* own = still(sim, 0, 40.0f, 40.0f);
    osc::sim::UnitCommand attack;
    attack.type = CommandType::Attack;
    attack.target_id = enemy->entity_id();
    CHECK(sim.takes_command(*tank, attack));
    CHECK_FALSE(sim.takes_command(*factory, attack));
    auto dummy = std::make_unique<osc::sim::Weapon>();
    dummy->dummy = true;
    factory->add_weapon(std::move(dummy));
    CHECK_FALSE(sim.takes_command(*factory, attack));
    factory->add_weapon(std::make_unique<osc::sim::Weapon>());
    CHECK(sim.takes_command(*factory, attack));
    attack.target_id = own->entity_id();
    CHECK_FALSE(sim.takes_command(*tank, attack));
}

TEST_CASE("A unit with no weapon takes an attack order and drops it when it comes up", "[attack]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    two_armies(sim);
    Unit* eng = engineer(sim, 10.0f, 20.0f);
    eng->set_motion_type("RULEUMT_Land");
    Unit* tank = walker(sim, 12.0f, 10.0f);
    tank->set_motion_type("RULEUMT_Land");
    auto gun = std::make_unique<osc::sim::Weapon>();
    gun->max_range = 5.0f;
    tank->add_weapon(std::move(gun));
    const Unit* enemy = still(sim, 1, 40.0f, 10.0f);
    osc::sim::UnitCommand move;
    move.type = CommandType::Move;
    move.target_pos = {60.0f, 0.0f, 20.0f};
    osc::sim::UnitCommand attack;
    attack.type = CommandType::Attack;
    attack.target_id = enemy->entity_id();
    attack.target_pos = enemy->position();
    const std::vector<osc::u32> both{eng->entity_id(), tank->entity_id()};

    sim.route_command({eng->entity_id()}, move, true);
    sim.tick();
    CHECK(sim.route_command(both, attack, true) != 0);
    CHECK(eng->command_queue().size() == 1);
    sim.tick();
    CHECK(eng->command_queue().empty());
    CHECK(tank->command_queue().size() == 1);

    sim.route_command({eng->entity_id()}, move, true);
    sim.tick();
    sim.route_command(both, attack, false);
    REQUIRE(eng->command_queue().size() == 2);
    CHECK(eng->command_queue().back().type == CommandType::Attack);
    sim.tick();
    CHECK(eng->command_queue().size() == 2);
    for (int i = 0; i < 400 && eng->command_queue().size() == 2; ++i) {
        sim.tick();
    }
    sim.tick();
    CHECK(eng->command_queue().empty());
}

TEST_CASE("IssueAttack goes only to units with the Attack command", "[attack][lua]") {
    osc::lua::LuaState lua;
    SimState sim(lua.raw(), nullptr);
    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_sim_bindings(lua, sim);
    two_armies(sim);
    Unit* eng = engineer(sim, 10.0f, 10.0f);
    eng->set_motion_type("RULEUMT_Land");
    Unit* tank = walker(sim, 12.0f, 10.0f);
    tank->set_motion_type("RULEUMT_Land");
    tank->add_command_cap("RULEUCC_Attack");
    tank->add_weapon(std::make_unique<osc::sim::Weapon>());
    const Unit* enemy = still(sim, 1, 40.0f, 10.0f);
    lua_State* L = lua.raw();
    const auto global = [&](const char* name, const Unit* u) {
        lua_newtable(L);
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, static_cast<osc::sim::Entity*>(const_cast<Unit*>(u)));
        lua_rawset(L, -3);
        lua_setglobal(L, name);
    };
    global("eng", eng);
    global("tank", tank);
    global("enemy", enemy);
    const auto r = lua.do_string(R"(
        if IssueAttack({eng}, enemy) ~= nil then error('the engineer took it') end
        if IssueAttack({eng, tank}, enemy) == nil then error('the tank did not take it') end
    )");
    if (!r) {
        FAIL(r.error().message);
    }
    CHECK(eng->command_queue().empty());
    REQUIRE(tank->command_queue().size() == 1);
    CHECK(tank->command_queue().front().target_id == enemy->entity_id());
}

TEST_CASE("Each Issue order goes only to units with its command", "[lua]") {
    struct Row {
        const char* issue;
        const char* cap;
    };
    const std::vector<Row> rows = {
        {"function(u) return IssueMove(u, {20, 0, 20}) end", "RULEUCC_Move"},
        {"function(u) return IssueMoveOffFactory(u, {20, 0, 20}) end", "RULEUCC_Move"},
        {"function(u) return IssueAggressiveMove(u, {20, 0, 20}) end", "RULEUCC_Move"},
        {"function(u) return IssueFormMove(u, {20, 0, 20}, 'NoFormation', 0) end", "RULEUCC_Move"},
        {"function(u) return IssueFormAggressiveMove(u, {20, 0, 20}, 'NoFormation', 0) end",
         "RULEUCC_Move"},
        {"function(u) return IssuePatrol(u, {20, 0, 20}) end", "RULEUCC_Patrol"},
        {"function(u) return IssueFormPatrol(u, {20, 0, 20}, 'NoFormation', 0) end",
         "RULEUCC_Patrol"},
        {"function(u) return IssueGuard(u, friend) end", "RULEUCC_Guard"},
        {"function(u) return IssueAttack(u, enemy) end", "RULEUCC_Attack"},
        {"function(u) return IssueFormAttack(u, enemy, 'NoFormation', 0) end", "RULEUCC_Attack"},
        {"function(u) return IssueRepair(u, friend) end", "RULEUCC_Repair"},
        {"function(u) return IssueCapture(u, enemy) end", "RULEUCC_Capture"},
        {"function(u) return IssueReclaim(u, rock) end", "RULEUCC_Reclaim"},
        {"function(u) return IssueSacrifice(u, friend) end", "RULEUCC_Sacrifice"},
        {"function(u) return IssueOverCharge(u, enemy) end", "RULEUCC_Overcharge"},
        {"function(u) return IssueNuke(u, {20, 0, 20}) end", "RULEUCC_Nuke"},
        {"function(u) return IssueTactical(u, {20, 0, 20}) end", "RULEUCC_Tactical"},
        {"function(u) return IssueTeleport(u, {20, 0, 20}) end", "RULEUCC_Teleport"},
        {"function(u) return IssueFerry(u, {20, 0, 20}) end", "RULEUCC_Ferry"},
        {"function(u) return IssueTransportUnload(u, {20, 0, 20}) end", "RULEUCC_Transport"},
    };
    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.issue) {
            osc::lua::LuaState lua;
            SimState sim(lua.raw(), nullptr);
            osc::lua::register_moho_bindings(lua, sim);
            osc::lua::register_sim_bindings(lua, sim);
            two_armies(sim);
            Unit* without = still(sim, 0, 10.0f, 10.0f);
            without->set_motion_type("RULEUMT_Land");
            Unit* with = still(sim, 0, 12.0f, 10.0f);
            with->set_motion_type("RULEUMT_Land");
            with->add_command_cap(row.cap);
            for (Unit* u : {without, with}) {
                for (const char* category : {"REPAIR", "CAPTURE", "RECLAIM"}) {
                    u->add_category(category);
                }
            }
            lua_State* L = lua.raw();
            const auto global = [&](const char* name, osc::sim::Entity* e) {
                lua_newtable(L);
                lua_pushstring(L, "_c_object");
                lua_pushlightuserdata(L, e);
                lua_rawset(L, -3);
                lua_setglobal(L, name);
            };
            global("without", without);
            global("with", with);
            global("friend", still(sim, 0, 14.0f, 10.0f));
            global("enemy", still(sim, 1, 40.0f, 10.0f));
            global("rock", rock(sim, 20.0f, 20.0f, 10.0f, 0.0f));
            const auto r = lua.do_string(std::string("local issue = ") + row.issue + R"(
                if issue({without}) ~= nil then error('a unit without the command took it') end
                issue({with})
            )");
            if (!r) {
                FAIL(r.error().message);
            }
            CHECK(without->command_queue().empty());
            CHECK_FALSE(with->command_queue().empty());
        }
    }
}

TEST_CASE(
    "A factory that can't move takes no Move, Patrol or Guard, and alone takes IssueFactoryAssist",
    "[lua][factory]") {
    osc::lua::LuaState lua;
    SimState sim(lua.raw(), nullptr);
    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_sim_bindings(lua, sim);
    two_armies(sim);
    Unit* factory = still(sim, 0, 10.0f, 10.0f);
    Unit* other = still(sim, 0, 20.0f, 10.0f);
    for (Unit* u : {factory, other}) {
        u->add_category("FACTORY");
        u->add_command_cap("RULEUCC_Move");
        u->add_command_cap("RULEUCC_Patrol");
        u->add_command_cap("RULEUCC_Guard");
    }
    Unit* eng = still(sim, 0, 14.0f, 10.0f);
    eng->set_motion_type("RULEUMT_Land");
    eng->add_command_cap("RULEUCC_Guard");
    lua_State* L = lua.raw();
    const auto global = [&](const char* name, const Unit* u) {
        lua_newtable(L);
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, static_cast<osc::sim::Entity*>(const_cast<Unit*>(u)));
        lua_rawset(L, -3);
        lua_setglobal(L, name);
    };
    global("factory", factory);
    global("other", other);
    global("eng", eng);
    const auto r = lua.do_string(R"(
        if IssueMove({factory}, {30, 0, 30}) ~= nil then error('move') end
        if IssuePatrol({factory}, {30, 0, 30}) ~= nil then error('patrol') end
        if IssueGuard({factory}, other) ~= nil then error('guard') end
        if IssueFactoryAssist({eng}, other) ~= nil then error('engineer assist') end
        IssueFactoryAssist({factory, eng}, other)
    )");
    if (!r) {
        FAIL(r.error().message);
    }
    REQUIRE(factory->command_queue().size() == 1);
    CHECK(factory->command_queue().front().type == CommandType::Guard);
    CHECK(eng->command_queue().empty());
}

TEST_CASE("Only Move, Attack, the form moves, Dive and a rally point return their command",
          "[lua]") {
    struct Row {
        const char* issue;
        const char* cap;
        bool returns_command;
    };
    const std::vector<Row> rows = {
        {"IssueMove({with}, {20, 0, 20})", "RULEUCC_Move", true},
        {"IssueMoveOffFactory({with}, {20, 0, 20})", "RULEUCC_Move", true},
        {"IssueAggressiveMove({with}, {20, 0, 20})", "RULEUCC_Move", true},
        {"IssueFormMove({with}, {20, 0, 20}, 'NoFormation', 0)", "RULEUCC_Move", true},
        {"IssueFormAggressiveMove({with}, {20, 0, 20}, 'NoFormation', 0)", "RULEUCC_Move", true},
        {"IssueAttack({with}, enemy)", "RULEUCC_Attack", true},
        {"IssueDive({with})", "RULEUCC_Dive", true},
        {"IssueFactoryRallyPoint({factory}, {20, 0, 20})", "RULEUCC_Move", true},
        {"IssueFormAttack({with}, enemy, 'NoFormation', 0)", "RULEUCC_Attack", false},
        {"IssuePatrol({with}, {20, 0, 20})", "RULEUCC_Patrol", false},
        {"IssueFormPatrol({with}, {20, 0, 20}, 'NoFormation', 0)", "RULEUCC_Patrol", false},
        {"IssueGuard({with}, friend)", "RULEUCC_Guard", false},
        {"IssueFactoryAssist({factory}, other)", "RULEUCC_Guard", false},
        {"IssueRepair({with}, friend)", "RULEUCC_Repair", false},
        {"IssueCapture({with}, enemy)", "RULEUCC_Capture", false},
        {"IssueReclaim({with}, rock)", "RULEUCC_Reclaim", false},
        {"IssueSacrifice({with}, friend)", "RULEUCC_Sacrifice", false},
        {"IssueOverCharge({with}, enemy)", "RULEUCC_Overcharge", false},
        {"IssueNuke({with}, {20, 0, 20})", "RULEUCC_Nuke", false},
        {"IssueTactical({with}, {20, 0, 20})", "RULEUCC_Tactical", false},
        {"IssueTeleport({with}, {20, 0, 20})", "RULEUCC_Teleport", false},
        {"IssueFerry({with}, {20, 0, 20})", "RULEUCC_Ferry", false},
        {"IssueTransportUnload({with}, {20, 0, 20})", "RULEUCC_Transport", false},
        {"IssueUpgrade({with}, 'uel0001')", "RULEUCC_Move", false},
        {"IssueScript({with}, {TaskName = 'EnhanceTask'})", "RULEUCC_Move", false},
        {"IssueBuildMobile({with}, {20, 0, 20}, 'uel0001', {})", "RULEUCC_Move", false},
        {"IssueBuildFactory({with}, 'uel0001', 1)", "RULEUCC_Move", false},
    };
    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.issue) {
            osc::lua::LuaState lua;
            SimState sim(lua.raw(), nullptr);
            osc::lua::register_moho_bindings(lua, sim);
            osc::lua::register_sim_bindings(lua, sim);
            two_armies(sim);
            Unit* with = still(sim, 0, 12.0f, 10.0f);
            with->set_motion_type("RULEUMT_Land");
            with->add_command_cap(row.cap);
            for (const char* category : {"ENGINEER", "REPAIR", "CAPTURE", "RECLAIM"}) {
                with->add_category(category);
            }
            Unit* factory = still(sim, 0, 30.0f, 10.0f);
            Unit* other = still(sim, 0, 40.0f, 10.0f);
            for (Unit* u : {factory, other}) {
                u->add_category("FACTORY");
                u->add_command_cap(row.cap);
            }
            lua_State* L = lua.raw();
            const auto global = [&](const char* name, osc::sim::Entity* e) {
                lua_newtable(L);
                lua_pushstring(L, "_c_object");
                lua_pushlightuserdata(L, e);
                lua_rawset(L, -3);
                lua_setglobal(L, name);
            };
            global("with", with);
            global("factory", factory);
            global("other", other);
            global("friend", still(sim, 0, 14.0f, 10.0f));
            global("enemy", still(sim, 1, 60.0f, 10.0f));
            global("rock", rock(sim, 20.0f, 20.0f, 10.0f, 0.0f));
            const auto r = lua.do_string(std::string(R"(
                local function results(...) return arg.n, arg[1] end
                n, command = results()") +
                                         row.issue + ")");
            if (!r) {
                FAIL(r.error().message);
            }
            const bool taken = !with->command_queue().empty() ||
                               !factory->command_queue().empty() ||
                               !factory->rally_orders().empty();
            CHECK(taken);
            lua_getglobal(L, "n");
            CHECK(lua_tonumber(L, -1) == (row.returns_command ? 1 : 0));
            lua_pop(L, 1);
            if (row.returns_command) {
                const auto done = lua.do_string("assert(not IsCommandDone(command))");
                if (!done) {
                    FAIL(done.error().message);
                }
            }
        }
    }
}

TEST_CASE("Repair, Sacrifice, Reclaim and Capture leave their target out of the units ordered",
          "[lua]") {
    const std::vector<std::pair<const char*, const char*>> rows = {
        {"IssueRepair", "RULEUCC_Repair"},
        {"IssueSacrifice", "RULEUCC_Sacrifice"},
        {"IssueReclaim", "RULEUCC_Reclaim"},
        {"IssueCapture", "RULEUCC_Capture"},
    };
    for (const auto& [issue, cap] : rows) {
        DYNAMIC_SECTION(issue) {
            osc::lua::LuaState lua;
            SimState sim(lua.raw(), nullptr);
            osc::lua::register_moho_bindings(lua, sim);
            osc::lua::register_sim_bindings(lua, sim);
            two_armies(sim);
            Unit* a = still(sim, 0, 10.0f, 10.0f);
            Unit* b = still(sim, 0, 12.0f, 10.0f);
            for (Unit* u : {a, b}) {
                u->set_motion_type("RULEUMT_Land");
                u->add_command_cap(cap);
                for (const char* category : {"RECLAIMABLE", "REPAIR", "CAPTURE", "RECLAIM"}) {
                    u->add_category(category);
                }
            }
            lua_State* L = lua.raw();
            const auto global = [&](const char* name, osc::sim::Entity* e) {
                lua_newtable(L);
                lua_pushstring(L, "_c_object");
                lua_pushlightuserdata(L, e);
                lua_rawset(L, -3);
                lua_setglobal(L, name);
            };
            global("a", a);
            global("b", b);
            const auto r = lua.do_string(std::string(issue) + "({a}, a) " + issue + "({a, b}, a)");
            if (!r) {
                FAIL(r.error().message);
            }
            CHECK(a->command_queue().empty());
            REQUIRE(b->command_queue().size() == 1);
            CHECK(b->command_queue().front().target_id == a->entity_id());
        }
    }
}
