// The sync checksum's coverage: state that decides what a unit does next
// is fingerprinted, in the domain a desync report names for it (two states
// that differ in it don't share a checksum), and a game saved with it set,
// restored and played on matches the one that went on.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "sim/army_brain.hpp"
#include "sim/collision_cells.hpp"
#include "sim/economy_event.hpp"
#include "sim/entity.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/platoon.hpp"
#include "sim/saved_game.hpp"
#include "sim/sim_snapshot.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"
#include "sim/world_snapshot.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <fmt/format.h>

#include <bit>
#include <functional>
#include <memory>
#include <sstream>
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
std::vector<osc::sim::EntityId> setup(SimState& sim) {
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    std::vector<osc::sim::EntityId> ids;
    for (int i = 0; i < 2; ++i) {
        auto u = std::make_unique<Unit>();
        u->set_army(i);
        u->set_max_speed(4.0f);
        u->set_position({static_cast<osc::f32>(i * 200), 0.0f, 0.0f});
        auto gun = std::make_unique<osc::sim::Weapon>();
        gun->target_priorities = {osc::sim::parse_category_list("ALLUNITS")};
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

Unit& unit(SimState& sim, osc::sim::EntityId id) {
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
            {"banked mass production", "units", [&] { u.economy().produced_mass = 1; }},
            {"banked energy production", "units", [&] { u.economy().produced_energy = 5; }},
            {"banked mass reclaim", "units", [&] { u.economy().reclaimed_mass = 2; }},
            {"banked energy reclaim", "units", [&] { u.economy().reclaimed_energy = 6; }},
            {"mass spent", "units", [&] { u.economy().spent_mass = 1; }},
            {"energy spent", "units", [&] { u.economy().spent_energy = 5; }},
            {"a consumption grant held", "units",
             [&] { u.economy().consumption_request.granted.energy = 0.5f; }},
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
            {"a reclaim's ticks before it takes its first share", "units",
             [&] { u.set_reclaim_wait(2); }},
            {"a silo's blocks preset for its next missile", "units",
             [&] { u.set_silo_blocks(1050); }},
            {"builder arm off target", "units", [&] { u.set_builder_on_target(false); }},
            {"an order waiting for its builder arm", "units", [&] { u.awaits_arm(); }},
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
            {"guard anchor", "units", [&] { u.set_guard_anchor({true, 3, {12, 9, 13, 10}}); }},
            {"vertical event", "units", [&] { u.set_vert_event("Bottom", nullptr); }},
            {"flying", "units", [&] { u.air_combat().flying = true; }},
            {"airframe velocity", "units",
             [&] {
                 u.air_combat().flying = true;
                 u.set_air_velocity(osc::sim::Vector3{3, 0, 1});
             }},
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
            {"a patrol leg's start", "orders",
             [&] { order([](UnitCommand& c) { c.patrol_from = {3.0f, 0.0f, 4.0f}; }); }},
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

TEST_CASE("A script thread shows in the trace's threads, not in the compared checksum",
          "[sync][checksum]") {
    // Retail's sim scripts fork threads by the focus army (aibrain.lua's
    // voice-overs), which each client sets for itself.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    setup(sim);
    const SimState::ChecksumParts before = sim.checksum_parts();
    REQUIRE(luaL_loadbuffer(g.L, "coroutine.yield()", 17, "=voice") == 0);
    sim.thread_manager().fork_thread(g.L);
    lua_settop(g.L, 0);
    const SimState::ChecksumParts after = sim.checksum_parts();
    CHECK(after.threads != before.threads);
    CHECK(after.values() == before.values());
    CHECK(after.total() == before.total());
}

TEST_CASE("The checksum trace keeps the threads column, after the compared domains",
          "[sync][checksum]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    setup(sim);
    std::ostringstream trace;
    sim.set_checksum_trace(&trace);
    sim.tick();

    std::istringstream lines(trace.str());
    std::string header, line;
    REQUIRE(std::getline(lines, header));
    REQUIRE(std::getline(lines, line));
    const auto words = [](const std::string& text) {
        std::vector<std::string> out;
        std::istringstream in(text);
        for (std::string w; in >> w;) out.push_back(w);
        return out;
    };
    const auto names = words(header);
    const auto values = words(line);
    REQUIRE(names.size() == SimState::ChecksumParts::kCount + 4); // # tick total ... threads
    CHECK(names.back() == "threads");
    REQUIRE(values.size() == names.size() - 1);
    const SimState::ChecksumParts& parts = sim.beat_checksum();
    CHECK(values[1] == fmt::format("{:08x}", parts.total()));
    CHECK(values.back() == fmt::format("{:016x}", parts.threads));
}

TEST_CASE("A game saved with that state, restored and played on, matches", "[sync][checksum]") {
    const auto mid_game = [](SimState& sim, const std::vector<osc::sim::EntityId>& ids) {
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
        u.economy().natural_producer = true;
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
        u.set_guard_anchor({true, u.command_queue().front().command_id, {12, 9, 13, 10}});
        UnitCommand attack; // queued behind it, its facing clock running
        attack.type = CommandType::Attack;
        attack.target_id = ids[1];
        attack.facing_clock = 5;
        u.push_command(attack, false);
        osc::sim::EconomyEvent* drain = sim.economy_events().create(ids[0], 0.0, 10.0, 1.0);
        drain->step();
        drain->step();
        drain->request().granted.energy = 0.5f;
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
    CHECK(u.economy().natural_producer);
    CHECK(b.get_army(0)->find_platoon_by_name("keepme") != nullptr);
    CHECK(u.idle_landing().descending);
    CHECK(u.idle_landing().layer == "Land");
    CHECK(u.guard_anchor().held);
    CHECK(u.guard_anchor().reserved.x1 == 13);
    CHECK(u.vert_event() == "Down");
    CHECK(u.air_combat().circle_reverse);
    CHECK(u.command_queue().front().formed);
    REQUIRE(b.economy_events().count() == 1);
    b.economy_events().for_each([](const osc::sim::EconomyEvent& drain) {
        CHECK(drain.progress() == Catch::Approx(0.2));
        CHECK(drain.request().granted.energy == 0.5f);
    });
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

TEST_CASE("Banked production and reclaim reports survive a snapshot",
          "[economy][snapshot][banked]") {
    LuaGuard ga;
    SimState a(ga.L, nullptr);
    const auto ids = setup(a);
    auto& u = unit(a, ids[0]);
    auto& econ = u.economy();
    econ.production_active = true;
    econ.production_mass = 4;
    econ.production_energy = 20;
    econ.consumption_active = true;
    econ.consumption_energy = 10;
    econ.consumption_request.granted.energy = 0.25f;
    econ.reclaim_mass = 2;
    econ.reclaim_energy = 6;
    u.bank_reclaim(a.get_army(0));
    u.manage_resources(a.get_army(0));
    // Saved after a callback changes the next tick's configured rates.
    econ.production_mass = 9;
    econ.production_energy = 90;
    econ.reclaim_mass = 8;
    econ.reclaim_energy = 80;
    const auto save = osc::sim::save_game(a, "banked production");
    REQUIRE_FALSE(save.snapshot.empty());

    LuaGuard gb;
    SimState b(gb.L, nullptr);
    REQUIRE(setup(b) == ids);
    const std::string error = osc::sim::load_snapshot(b, save.snapshot);
    INFO(error);
    REQUIRE(error.empty());
    osc::sim::WorldSnapshot world;
    osc::sim::capture_world(b, world);
    const auto* record = world.find(ids[0]);
    REQUIRE(record);
    CHECK(record->mass_produced == 3);
    CHECK(record->energy_produced == 11);
}

// The registry's own order: which entities a rect query finds first, which
// step first, and which ids come next. None of it shows in any entity's
// place, so two games alike entity by entity can still part from here on.
// The checksum must tell them apart, and a snapshot must carry it.

namespace {

/// A sim as `setup` makes it, with collision cells, after a few beats.
std::vector<osc::sim::EntityId> setup_played(SimState& sim) {
    sim.set_seed(7);
    sim.entity_registry().init_spatial_grid(512, 512);
    auto ids = setup(sim);
    sim.set_recording(true);
    for (int i = 0; i < 3; ++i) sim.tick();
    return ids;
}

/// A unit of `army` whose box (half a unit each way) is in the collision
/// cells at `at`.
osc::sim::EntityId add_boxed(SimState& sim, int army, const osc::sim::Vector3& at) {
    auto u = std::make_unique<Unit>();
    u->set_army(army);
    u->set_position(at);
    osc::sim::CollisionShape box;
    box.type = osc::sim::CollisionShapeType::BOX;
    box.sx = 0.5f;
    box.sy = 0.5f;
    box.sz = 0.5f;
    u->set_default_collision_shape(box);
    return sim.entity_registry().register_entity(std::move(u));
}

/// What a rect query over the cell (100, 100) is in finds, in its order.
std::vector<osc::sim::EntityId> cell_order(SimState& sim) {
    std::vector<osc::sim::EntityId> ids;
    const auto rect = osc::sim::CollisionCells::query_rect(100.0f, 100.0f, 103.0f, 103.0f);
    for (const osc::sim::Entity* e :
         sim.entity_registry().collision_cells().gather(rect, true, true, true)) {
        ids.push_back(e->entity_id());
    }
    return ids;
}

/// The bits of each entity's place, in id order: what equal transforms are.
std::vector<osc::u32> places(SimState& sim) {
    std::vector<osc::u32> out;
    sim.entity_registry().for_each([&](const osc::sim::Entity& e) {
        for (const osc::f32 v : {e.position().x, e.position().y, e.position().z, e.orientation().x,
                                 e.orientation().y, e.orientation().z, e.orientation().w}) {
            out.push_back(std::bit_cast<osc::u32>(v));
        }
    });
    return out;
}

/// Load `save` into a sim set up as `setup_played` does, before its first
/// beat (a snapshot loads only into a sim that hasn't run).
void restore(SimState& sim, const osc::sim::SavedGame& save,
             const std::vector<osc::sim::EntityId>& ids) {
    sim.set_seed(7);
    sim.entity_registry().init_spatial_grid(512, 512);
    REQUIRE(setup(sim) == ids);
    const std::string err = osc::sim::load_snapshot(sim, save.snapshot);
    INFO(err);
    REQUIRE(err.empty());
    sim.adopt_history(save.game);
}

/// Play `a` and `b` on beat by beat: their checksums agree on each.
void play_on_together(SimState& a, SimState& b, int beats) {
    for (int i = 0; i < beats; ++i) {
        a.tick();
        b.tick();
        INFO("beat " << a.beat_count());
        REQUIRE(changed_domains(a.checksum_parts(), b.checksum_parts()).empty());
    }
}

} // namespace

TEST_CASE("The collision cells' order changes the sync checksum, though nothing moved",
          "[sync][checksum][collision_cells]") {
    LuaGuard ga, gb;
    SimState a(ga.L, nullptr), b(gb.L, nullptr);
    auto ids = setup_played(a);
    REQUIRE(setup_played(b) == ids);
    // Two units in one cell, a box apart, in both games
    for (SimState* sim : {&a, &b}) {
        const auto first = add_boxed(*sim, 0, {100.5f, 0.0f, 100.5f});
        const auto second = add_boxed(*sim, 1, {102.5f, 0.0f, 100.5f});
        if (sim == &a) {
            ids.push_back(first);
            ids.push_back(second);
        }
    }
    REQUIRE(changed_domains(a.checksum_parts(), b.checksum_parts()).empty());
    REQUIRE(cell_order(a) == std::vector{ids[3], ids[2]}); // the last linked first

    // In b the first leaves its cells and comes back where it was: a rect
    // query finds it first from now on, and nothing has moved
    const auto before = places(b);
    auto& cells = b.entity_registry().collision_cells();
    osc::sim::Entity& relinked = *b.entity_registry().find(ids[2]);
    cells.unlink(relinked);
    cells.link(relinked);
    CHECK(places(b) == before);
    CHECK(places(b) == places(a));
    CHECK(cell_order(b) == std::vector{ids[2], ids[3]});
    CHECK(changed_domains(a.checksum_parts(), b.checksum_parts()) == "entities");

    // Saved, restored and played on, its order holds and its checksum agrees
    const osc::sim::SavedGame save = osc::sim::save_game(b, "relinked");
    REQUIRE_FALSE(save.snapshot.empty());
    LuaGuard gc;
    SimState c(gc.L, nullptr);
    restore(c, save, std::vector(ids.begin(), ids.begin() + 2));
    CHECK(cell_order(c) == cell_order(b));
    CHECK(c.compute_sync_checksum() == b.compute_sync_checksum());
    play_on_together(b, c, 30);
    CHECK(cell_order(c) == cell_order(b));
    // A link made after the load comes in the order the saved game's would
    const auto late_b = add_boxed(b, 0, {101.5f, 0.0f, 101.5f});
    const auto late_c = add_boxed(c, 0, {101.5f, 0.0f, 101.5f});
    CHECK(late_c == late_b);
    CHECK(cell_order(c) == cell_order(b));
    CHECK(cell_order(c).front() == late_c);
    CHECK(changed_domains(b.checksum_parts(), c.checksum_parts()).empty());
}

TEST_CASE("The order entities step in changes the sync checksum, and a snapshot keeps it",
          "[sync][checksum]") {
    LuaGuard ga, gb;
    SimState a(ga.L, nullptr), b(gb.L, nullptr);
    const auto ids = setup_played(a);
    REQUIRE(setup_played(b) == ids);
    REQUIRE(changed_domains(a.checksum_parts(), b.checksum_parts()).empty());

    // b's two units step the other way round from its next beat on
    std::vector<osc::sim::EntityId>& walk = b.entity_registry().walk();
    REQUIRE(walk == ids);
    std::swap(walk[0], walk[1]);
    CHECK(places(b) == places(a));
    CHECK(changed_domains(a.checksum_parts(), b.checksum_parts()) == "entities");

    const osc::sim::SavedGame save = osc::sim::save_game(b, "walk order");
    REQUIRE_FALSE(save.snapshot.empty());
    LuaGuard gc;
    SimState c(gc.L, nullptr);
    restore(c, save, ids);
    CHECK(c.entity_registry().walk() == std::vector{ids[1], ids[0]});
    CHECK(c.compute_sync_checksum() == b.compute_sync_checksum());
    play_on_together(b, c, 30);
}

TEST_CASE("The id pools' held serials change the sync checksum before any id comes back",
          "[sync][checksum]") {
    // An entity made and destroyed: its serial is held, then free again
    // (Moho's IdPool), and the next entity of its kind takes it
    const auto spend_one = [](SimState& sim) {
        auto e = std::make_unique<osc::sim::Entity>();
        e->set_army(0);
        const osc::sim::EntityId id = sim.entity_registry().register_entity(std::move(e));
        sim.entity_registry().destroy(*sim.entity_registry().find(id));
        return id;
    };
    const auto next_id = [](SimState& sim) {
        auto e = std::make_unique<osc::sim::Entity>();
        e->set_army(0);
        return sim.entity_registry().register_entity(std::move(e));
    };
    LuaGuard ga, gb, gn;
    SimState a(ga.L, nullptr), b(gb.L, nullptr), never(gn.L, nullptr);
    const auto ids = setup_played(a);
    REQUIRE(setup_played(b) == ids);
    REQUIRE(setup_played(never) == ids);

    // a spends one a beat before b does; `never` spends none
    const osc::sim::EntityId spent = spend_one(a);
    a.tick();
    b.tick();
    REQUIRE(spend_one(b) == spent);
    a.tick();
    b.tick();
    never.tick();
    never.tick();
    REQUIRE(a.entity_registry().find(spent) == nullptr);
    REQUIRE(b.entity_registry().find(spent) == nullptr);
    CHECK(places(a) == places(b));
    CHECK(places(a) == places(never));
    // Held a beat apart: the same live entities, ids free again a beat apart
    CHECK(changed_domains(a.checksum_parts(), b.checksum_parts()) == "entities");
    // Its pool's next serial is one on
    CHECK(changed_domains(a.checksum_parts(), never.checksum_parts()) == "entities");

    // Saved while b's serial is held; restored and played on past its hold,
    // the next entity takes the same id in both, the serial b spent
    const osc::sim::SavedGame save = osc::sim::save_game(b, "held serial");
    REQUIRE_FALSE(save.snapshot.empty());
    LuaGuard gc;
    SimState c(gc.L, nullptr);
    restore(c, save, ids);
    CHECK(c.compute_sync_checksum() == b.compute_sync_checksum());
    play_on_together(b, c, static_cast<int>(osc::sim::EntityRegistry::kIdHeldBeats) + 2);
    const osc::sim::EntityId reused = next_id(b);
    CHECK(next_id(c) == reused);
    CHECK(reused.moho() == spent.moho());
    CHECK(changed_domains(b.checksum_parts(), c.checksum_parts()).empty());
}

TEST_CASE("A paused beat ages the id pools' held serials, so the checksum counts it",
          "[sync][checksum]") {
    // Moho's Sim::Sync runs EntityDB::Purge (and so IdPool::Update) after
    // every beat, ticked or paused: how many beats a pause lasted decides
    // when a serial is free again. Two games alike but for a paused beat
    // are told apart, though nothing in them moved.
    LuaGuard ga, gb;
    SimState a(ga.L, nullptr), b(gb.L, nullptr);
    const auto ids = setup_played(a);
    REQUIRE(setup_played(b) == ids);
    for (SimState* sim : {&a, &b}) {
        auto e = std::make_unique<osc::sim::Entity>();
        e->set_army(0);
        const osc::sim::EntityId id = sim->entity_registry().register_entity(std::move(e));
        sim->entity_registry().destroy(*sim->entity_registry().find(id));
    }
    a.tick();
    b.tick();
    REQUIRE(changed_domains(a.checksum_parts(), b.checksum_parts()).empty());

    // b pauses for a beat that runs no tick, then plays on
    b.begin_command_sources();
    b.set_command_source(0, 0, true);
    b.set_pause_holds(true);
    b.request_pause(0);
    REQUIRE(b.paused());
    const auto ticks = b.tick_count();
    b.tick();
    REQUIRE(b.tick_count() == ticks);
    b.resume(0);
    REQUIRE_FALSE(b.paused());
    a.tick();
    b.tick();
    REQUIRE(a.tick_count() == b.tick_count());
    CHECK(places(a) == places(b));
    CHECK(changed_domains(a.checksum_parts(), b.checksum_parts()).find("entities") !=
          std::string::npos);
}
