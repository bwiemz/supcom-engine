// The sync checksum's coverage: state that decides what a unit does next
// is fingerprinted (two states that differ in it don't share a checksum),
// and a game saved with it set, restored and played on matches the one
// that went on.

#include <catch2/catch_test_macros.hpp>

#include "sim/army_brain.hpp"
#include "sim/manipulator.hpp"
#include "sim/platoon.hpp"
#include "sim/saved_game.hpp"
#include "sim/sim_snapshot.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lua.h>
}

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

/// Two armies, a gunned tank each, as every sim here starts.
std::vector<osc::u32> setup(SimState& sim) {
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    std::vector<osc::u32> ids;
    for (int i = 0; i < 2; ++i) {
        auto u = std::make_unique<Unit>();
        u->set_army(i);
        u->set_max_speed(4.0f);
        u->set_position({static_cast<osc::f32>(i * 200), 0.0f, 0.0f});
        auto gun = std::make_unique<osc::sim::Weapon>();
        gun->max_range = 20.0f;
        u->add_weapon(std::move(gun));
        ids.push_back(sim.entity_registry().register_entity(std::move(u)));
    }
    osc::sim::GameSetup game;
    game.scenario = "/maps/test/test_scenario.lua";
    game.seed = 7;
    sim.set_game_setup(game);
    return ids;
}

Unit& unit(SimState& sim, osc::u32 id) { return static_cast<Unit&>(*sim.entity_registry().find(id)); }

} // namespace

TEST_CASE("State that decides a unit's next move changes the sync checksum", "[sync][checksum]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    const auto ids = setup(sim);
    Unit& u = unit(sim, ids[0]);
    osc::sim::Weapon& w = *u.weapons().front();
    const std::vector<std::pair<const char*, std::function<void()>>> changes = {
        // A ground attack's shots decide when its order goes round the queue
        {"weapon shots at target", [&] { w.shots_at_target = 2; }},
        {"weapon ground order", [&] { w.ground_from_order = true; }},
        {"weapon last order point", [&] { w.last_order_point = osc::sim::Vector3{4, 0, 4}; }},
        {"weapon target check clock", [&] { w.target_check_clock = 7; }},
        {"weapon range (ChangeMaxRadius)", [&] { w.max_range = 30.0f; }},
        {"weapon rate of fire", [&] { w.rate_of_fire = 2.0f; }},
        {"weapon damage", [&] { w.damage = 55.0f; }},
        {"weapon target layers", [&] { w.fire_target_layer_caps = 0x3; }},
        {"attack facing", [&] { u.set_attack_facing({0, 0, 1}); }},
        {"statistics (KILLS)", [&] { u.set_stat("KILLS", 3.0); }},
        {"a second statistic", [&] { u.set_stat("DamageTaken", 40.0); }},
        {"unit state", [&] { u.set_unit_state("Busy", true); }},
        {"busy", [&] { u.set_busy(true); }},
        {"blocked command queue", [&] { u.set_block_command_queue(true); }},
        {"can't take damage", [&] { u.set_can_take_damage(false); }},
        {"can't be killed", [&] { u.set_can_be_killed(false); }},
        {"speed multiplier", [&] { u.set_speed_mult(0.5f); }},
        {"veterancy", [&] { u.set_vet_level(2); }},
        {"last attacker", [&] { u.set_last_attacker_id(ids[1]); }},
        {"enhancement", [&] { u.add_enhancement("Back", "Shield"); }},
        {"heading", [&] { u.set_heading(1.25f); }},
        {"elevation target", [&] { u.set_elevation_target(25.0f); }},
        {"an attack's facing clock",
         [&] {
             UnitCommand c;
             c.type = CommandType::Attack;
             c.target_id = ids[1];
             u.push_command(c, true);
             const auto before = sim.compute_sync_checksum();
             UnitCommand clocked = c;
             clocked.facing_clock = 5;
             u.push_command(clocked, true);
             CHECK(sim.compute_sync_checksum() != before);
         }},
        {"a platoon", [&] { sim.get_army(0)->create_platoon("Label"); }},
        {"its unique name", [&] { sim.get_army(0)->platoon_at(0)->set_unique_name("KeepMe"); }},
        {"its DisbandOnIdle", [&] { sim.get_army(0)->platoon_at(0)->set_disband_on_idle(); }},
        {"its unit", [&] { sim.get_army(0)->platoon_at(0)->add_unit(ids[0]); }},
        {"a collision detector",
         [&] {
             auto d = std::make_unique<osc::sim::CollisionDetectorManipulator>();
             d->set_enabled(false);
             d->watch_bone(0);
             u.add_manipulator(std::move(d));
         }},
        {"its bone's contact",
         [&] {
             for (const auto& m : u.manipulators())
                 if (auto* d = dynamic_cast<osc::sim::CollisionDetectorManipulator*>(m.get()))
                     d->watched().front().below_foot_height = true;
         }},
    };
    for (const auto& [what, change] : changes) {
        const auto before = sim.compute_sync_checksum();
        change();
        INFO(what);
        CHECK(sim.compute_sync_checksum() != before);
    }
}

TEST_CASE("A game saved with that state, restored and played on, matches", "[sync][checksum]") {
    const auto mid_game = [](SimState& sim, const std::vector<osc::u32>& ids) {
        Unit& u = unit(sim, ids[0]);
        osc::sim::Weapon& w = *u.weapons().front();
        w.shots_at_target = 2;
        w.ground_from_order = true;
        w.last_order_point = osc::sim::Vector3{4, 0, 4};
        w.max_range = 30.0f;
        u.set_attack_facing({0, 0, 1});
        u.set_stat("KILLS", 3.0);
        u.set_unit_state("Busy", true);
        u.set_speed_mult(0.75f);
        u.set_vet_level(1);
        u.add_enhancement("Back", "Shield");
        auto* p = sim.get_army(0)->create_platoon("Label");
        p->set_unique_name("KeepMe");
        p->add_unit(ids[0]);
        auto d = std::make_unique<osc::sim::CollisionDetectorManipulator>();
        d->watch_bone(0);
        d->watched().front().below_foot_height = true;
        u.add_manipulator(std::move(d));
        UnitCommand move;
        move.type = CommandType::Move;
        move.target_pos = {60.0f, 0.0f, 30.0f};
        u.push_command(move, true);
    };
    LuaGuard ga;
    SimState a(ga.L, nullptr);
    a.set_seed(7);
    const auto ids = setup(a);
    a.set_recording(true);
    for (int i = 0; i < 5; ++i) a.tick();
    mid_game(a, ids);
    a.tick();
    const osc::sim::SavedGame save = osc::sim::save_game(a, "nondefault");
    REQUIRE_FALSE(save.snapshot.empty());
    for (int i = 0; i < 30; ++i) a.tick();

    LuaGuard gb;
    SimState b(gb.L, nullptr);
    b.set_seed(7);
    REQUIRE(setup(b) == ids);
    const std::string err = osc::sim::load_snapshot(b, save.snapshot);
    INFO(err);
    REQUIRE(err.empty());
    b.adopt_history(save.game);
    CHECK(b.compute_sync_checksum() == save.game.checksums.back());
    // What was set came back
    const Unit& u = unit(b, ids[0]);
    CHECK(u.weapons().front()->shots_at_target == 2);
    CHECK(u.get_stat("KILLS") == 3.0);
    CHECK(b.get_army(0)->find_platoon_by_name("keepme") != nullptr);
    for (int i = 0; i < 30; ++i) b.tick();
    CHECK(b.compute_sync_checksum() == a.compute_sync_checksum());
}
