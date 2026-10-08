// The sync checksum's coverage: state that decides what a unit does next
// is fingerprinted, in the domain a desync report names for it (two states
// that differ in it don't share a checksum), and a game saved with it set,
// restored and played on matches the one that went on.

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

Unit& unit(SimState& sim, osc::u32 id) {
    return static_cast<Unit&>(*sim.entity_registry().find(id));
}

/// The checksum's domains that differ between `a` and `b`, by name.
std::string changed_domains(const SimState::ChecksumParts& a, const SimState::ChecksumParts& b) {
    std::string out;
    const auto before = a.values();
    const auto after = b.values();
    for (size_t i = 0; i < before.size(); ++i) {
        if (before[i] == after[i]) continue;
        if (!out.empty()) out += ' ';
        out += SimState::ChecksumParts::kNames[i];
    }
    return out;
}

struct Change {
    const char* what;
    const char* domain; ///< the one domain it changes
    std::function<void()> apply;
};

/// Apply each change in turn: each changes its domain, and only that.
void check_changes(const SimState& sim, const std::vector<Change>& changes) {
    for (const Change& c : changes) {
        const SimState::ChecksumParts before = sim.checksum_parts();
        c.apply();
        INFO(c.what);
        CHECK(changed_domains(before, sim.checksum_parts()) == c.domain);
    }
}

} // namespace

TEST_CASE("State that decides a unit's next move changes the sync checksum, in its domain",
          "[sync][checksum]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    const auto ids = setup(sim);
    Unit& u = unit(sim, ids[0]);
    osc::sim::Weapon& w = *u.weapons().front();
    const auto order = [&](const std::function<void(UnitCommand&)>& set) {
        UnitCommand c;
        c.type = CommandType::Move;
        c.target_pos = {40, 0, 40};
        set(c);
        u.push_command(c, true);
    };
    const auto landing = [&](const std::function<void(Unit::IdleLanding&)>& set) {
        Unit::IdleLanding l = u.idle_landing();
        set(l);
        u.set_idle_landing(l);
    };
    check_changes(
        sim,
        {
            // A ground attack's shots decide when its order goes round the queue
            {"weapon shots at target", "weapons", [&] { w.shots_at_target = 2; }},
            {"weapon ground order", "weapons", [&] { w.ground_from_order = true; }},
            {"weapon last order point", "weapons",
             [&] { w.last_order_point = osc::sim::Vector3{4, 0, 4}; }},
            {"weapon target check clock", "weapons", [&] { w.target_check_clock = 7; }},
            {"weapon range (ChangeMaxRadius)", "weapons", [&] { w.max_range = 30.0f; }},
            {"weapon minimum range", "weapons", [&] { w.min_range = 3.0f; }},
            {"weapon damage radius", "weapons", [&] { w.damage_radius = 2.0f; }},
            {"weapon rate of fire", "weapons", [&] { w.rate_of_fire = 2.0f; }},
            {"weapon damage", "weapons", [&] { w.damage = 55.0f; }},
            {"weapon target layers", "weapons", [&] { w.fire_target_layer_caps = 0x3; }},
            {"attack facing", "units", [&] { u.set_attack_facing({0, 0, 1}); }},
            {"statistics (KILLS)", "units", [&] { u.set_stat("KILLS", 3.0); }},
            {"a second statistic", "units", [&] { u.set_stat("DamageTaken", 40.0); }},
            {"unit state", "units", [&] { u.set_unit_state("Busy", true); }},
            {"busy", "units", [&] { u.set_busy(true); }},
            {"blocked command queue", "units", [&] { u.set_block_command_queue(true); }},
            {"can't take damage", "units", [&] { u.set_can_take_damage(false); }},
            {"can't be killed", "units", [&] { u.set_can_be_killed(false); }},
            {"jostled", "units", [&] { u.set_jostled(true); }},
            {"speed multiplier", "units", [&] { u.set_speed_mult(0.5f); }},
            {"acceleration multiplier", "units", [&] { u.set_accel_mult(0.5f); }},
            {"turn multiplier", "units", [&] { u.set_turn_mult(0.5f); }},
            {"veterancy", "units", [&] { u.set_vet_level(2); }},
            {"a silo's blocks preset for its next missile", "units",
             [&] { u.set_silo_blocks(1050); }},
            {"last attacker", "units", [&] { u.set_last_attacker_id(ids[1]); }},
            {"enhancement", "units", [&] { u.add_enhancement("Back", "Shield"); }},
            {"heading", "navigation", [&] { u.set_heading(1.25f); }},
            {"elevation target", "navigation", [&] { u.set_elevation_target(25.0f); }},
            // An idle aircraft's landing (AutoLandTime) and its flight
            {"idle since", "units", [&] { landing([](auto& l) { l.idle_since = 40; }); }},
            {"descending", "units", [&] { landing([](auto& l) { l.descending = true; }); }},
            {"landing place", "units",
             [&] { landing([](auto& l) { l.target = osc::sim::Vector3{12, 0, 9}; }); }},
            {"landing layer", "units", [&] { landing([](auto& l) { l.layer = "Water"; }); }},
            {"landing reservation", "units",
             [&] { landing([](auto& l) { l.reserved = {10, 7, 14, 11}; }); }},
            {"vertical event", "units", [&] { u.set_vert_event("Bottom", nullptr); }},
            {"flying", "units", [&] { u.air_combat().flying = true; }},
            {"airframe velocity", "units",
             [&] { u.air_combat().velocity = osc::sim::Vector3{3, 0, 1}; }},
            {"circle anchor", "units",
             [&] { u.air_combat().circle_anchor = osc::sim::Vector3{5, 0, 5}; }},
            // Orders
            {"a move", "orders", [&] { order([](UnitCommand&) {}); }},
            {"its formation", "orders",
             [&] { order([](UnitCommand& c) { c.formation = "GrowthFormation"; }); }},
            {"its formation slot", "orders",
             [&] {
                 order([](UnitCommand& c) {
                     c.formation = "GrowthFormation";
                     c.formed = true;
                 });
             }},
            {"its formation pace", "orders",
             [&] {
                 order([](UnitCommand& c) {
                     c.formation = "GrowthFormation";
                     c.formed = true;
                     c.speed_cap = 2.5f;
                 });
             }},
            {"its formation facing", "orders",
             [&] {
                 order([](UnitCommand& c) {
                     c.formation = "GrowthFormation";
                     c.formed = true;
                     c.speed_cap = 2.5f;
                     c.has_facing = true;
                     c.facing = 90.0f;
                 });
             }},
            {"a winged attack within reach", "orders",
             [&] {
                 order([&](UnitCommand& c) {
                     c.type = CommandType::Attack;
                     c.target_id = ids[1];
                 });
                 UnitCommand c = u.command_queue().front();
                 c.engaged = true;
                 u.push_command(c, true);
             }},
            {"an attack's facing clock", "orders",
             [&] {
                 UnitCommand c = u.command_queue().front();
                 c.facing_clock = 5;
                 u.push_command(c, true);
             }},
            {"a patrol's look-about clock", "orders",
             [&] { order([](UnitCommand& c) { c.patrol_scan = 4; }); }},
            {"a raised factory build", "orders",
             [&] {
                 order([](UnitCommand& c) {
                     c.type = CommandType::BuildFactory;
                     c.blueprint_id = "tank";
                 });
                 UnitCommand c = u.command_queue().front();
                 c.count = 3;
                 c.max_count = 3;
                 u.push_command(c, true);
             }},
            {"its count down, its most kept", "orders",
             [&] {
                 UnitCommand c = u.command_queue().front();
                 c.count = 2;
                 u.push_command(c, true);
             }},
            {"a platoon", "armies", [&] { sim.get_army(0)->create_platoon("Label"); }},
            {"its unique name", "armies",
             [&] { sim.get_army(0)->platoon_at(0)->set_unique_name("KeepMe"); }},
            {"its DisbandOnIdle", "armies",
             [&] { sim.get_army(0)->platoon_at(0)->set_disband_on_idle(); }},
            {"its unit", "armies", [&] { sim.get_army(0)->platoon_at(0)->add_unit(ids[0]); }},
            {"a collision detector", "units",
             [&] {
                 auto d = std::make_unique<osc::sim::CollisionDetectorManipulator>();
                 d->set_enabled(false);
                 d->watch_bone(0);
                 u.add_manipulator(std::move(d));
             }},
            {"its bone's contact", "units",
             [&] {
                 for (const auto& m : u.manipulators())
                     if (auto* d = dynamic_cast<osc::sim::CollisionDetectorManipulator*>(m.get()))
                         d->watched().front().below_foot_height = true;
             }},
        });
}

TEST_CASE("A game saved with that state, restored and played on, matches", "[sync][checksum]") {
    const auto mid_game = [](SimState& sim, const std::vector<osc::u32>& ids) {
        Unit& u = unit(sim, ids[0]);
        osc::sim::Weapon& w = *u.weapons().front();
        w.shots_at_target = 2;
        w.ground_from_order = true;
        w.last_order_point = osc::sim::Vector3{4, 0, 4};
        w.max_range = 30.0f;
        w.min_range = 2.0f;
        w.damage_radius = 1.5f;
        w.rate_of_fire = 2.0f;
        w.damage = 30.0f;
        w.fire_target_layer_caps = 0x3;
        w.target_check_clock = 9;
        u.set_attack_facing({0, 0, 1});
        u.set_stat("KILLS", 3.0);
        u.set_unit_state("Busy", true);
        u.set_can_take_damage(false);
        u.set_can_be_killed(false);
        u.set_jostled(true);
        u.set_speed_mult(0.75f);
        u.set_accel_mult(0.5f);
        u.set_turn_mult(0.5f);
        u.set_heading(1.25f);
        u.set_elevation_target(25.0f);
        u.set_vet_level(1);
        u.set_last_attacker_id(ids[1]);
        u.add_enhancement("Back", "Shield");
        auto* p = sim.get_army(0)->create_platoon("Label");
        p->set_unique_name("KeepMe");
        p->add_unit(ids[0]);
        auto d = std::make_unique<osc::sim::CollisionDetectorManipulator>();
        d->watch_bone(0);
        d->watched().front().below_foot_height = true;
        u.add_manipulator(std::move(d));
        Unit::IdleLanding landing;
        landing.idle_since = 3;
        landing.descending = true;
        landing.target = {12, 0, 9};
        landing.layer = "Land";
        u.set_idle_landing(landing);
        u.set_vert_event("Down", nullptr);
        u.air_combat().circle_anchor = {5, 0, 5};
        u.air_combat().circle_reverse = true;
        UnitCommand move; // a formation's slot
        move.type = CommandType::Move;
        move.target_pos = {60.0f, 0.0f, 30.0f};
        move.formation = "GrowthFormation";
        move.formed = true;
        move.speed_cap = 3.0f;
        u.push_command(move, true);
        UnitCommand attack; // queued behind it, its facing clock running
        attack.type = CommandType::Attack;
        attack.target_id = ids[1];
        attack.facing_clock = 5;
        u.push_command(attack, false);
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
    CHECK(u.weapons().front()->min_range == 2.0f);
    CHECK(u.turn_mult() == 0.5f);
    CHECK(u.last_attacker_id() == ids[1]);
    CHECK(u.get_stat("KILLS") == 3.0);
    CHECK(b.get_army(0)->find_platoon_by_name("keepme") != nullptr);
    CHECK(u.idle_landing().descending);
    CHECK(u.idle_landing().layer == "Land");
    CHECK(u.vert_event() == "Down");
    CHECK(u.air_combat().circle_reverse);
    CHECK(u.command_queue().front().formed);
    // A collision detector's bone that was below its foot height still is:
    // a footfall or crash isn't told again after a load (roadmap item 5).
    const osc::sim::CollisionDetectorManipulator* detector = nullptr;
    for (const auto& m : u.manipulators())
        if (const auto* d = dynamic_cast<const osc::sim::CollisionDetectorManipulator*>(m.get()))
            detector = d;
    REQUIRE(detector);
    REQUIRE(detector->watched().size() == 1);
    CHECK(detector->watched().front().below_foot_height);
    for (int i = 0; i < 30; ++i) b.tick();
    CHECK(b.compute_sync_checksum() == a.compute_sync_checksum());
}
