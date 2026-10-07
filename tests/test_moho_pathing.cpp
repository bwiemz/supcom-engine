// Units moving as Moho's do (roadmap item 4c-2c): with the sim's switch on,
// a ground unit's move asks its army's path queue, and its navigator
// follows the cells (faf-re CAiNavigatorLand, CAiPathNavigator), driving at
// each target and refused ground it won't fit.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "blueprints/footprint.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/army_brain.hpp"
#include "sim/occupancy.hpp"
#include "sim/path_navigator.hpp"
#include "sim/path_tables.hpp"
#include "sim/saved_game.hpp"
#include "sim/sim_snapshot.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using osc::f32;
using osc::i32;
using osc::u32;
using osc::u8;
using osc::blueprints::NamedFootprint;
using osc::sim::Unit;
namespace oc = osc::blueprints::occupancy;

namespace {

constexpr u32 kMap = 128;

std::unique_ptr<osc::map::Terrain> flat_terrain() {
    std::vector<osc::u16> heights(static_cast<size_t>(kMap + 1) * (kMap + 1), 1280);
    osc::map::Heightmap hm(kMap, kMap, 1.0f / 128.0f, std::move(heights));
    return std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false);
}

NamedFootprint land_class(int size, const char* name) {
    NamedFootprint f;
    f.name = name;
    f.size_x = static_cast<u8>(size);
    f.size_z = static_cast<u8>(size);
    f.caps = oc::kLand;
    f.max_water_depth = 0.05f;
    f.max_slope = 0.75f;
    return f;
}

/// A sim on a flat 128 x 128 map with land classes 1x1 and 3x3, a 1x1 and a
/// 3x3 tank, and Moho pathing as `moho` says.
struct World {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    explicit World(bool moho) {
        osc::sim::GameSetup game;
        game.scenario = "/maps/test/test_scenario.lua";
        game.seed = 3;
        store.add_footprint_class(land_class(1, "Vehicle1x1"));
        store.add_footprint_class(land_class(3, "Vehicle3x3"));
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        sim.set_terrain(flat_terrain());
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.set_game_setup(game);
        sim.set_moho_pathing(moho);
        lua_State* L = state.raw();
        for (const char* bp : {
                 "{BlueprintId = 'tank', Categories = {'LAND', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, Footprint = {SizeX = 1, SizeZ = 1},"
                 " Physics = {MotionType = 'RULEUMT_Land', MaxSpeed = 4, MaxAcceleration = 4,"
                 "  MaxBrake = 4, TurnRate = 180}}",
                 "{BlueprintId = 'bigtank', Categories = {'LAND', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, Footprint = {SizeX = 3, SizeZ = 3},"
                 " Physics = {MotionType = 'RULEUMT_Land', MaxSpeed = 4, MaxAcceleration = 4,"
                 "  MaxBrake = 4, TurnRate = 180}}",
             }) {
            REQUIRE(state.do_string(std::string("return ") + bp).ok());
            store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
            lua_pop(L, 1);
        }
        store.expose_to_lua(L);
        REQUIRE(state
                    .do_string("Plain = setmetatable({}, {__index = moho.unit_methods})"
                               " Plain.__index = Plain")
                    .ok());
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        for (const char* id : {"tank", "bigtank"}) {
            lua_pushstring(L, id);
            lua_getglobal(L, "Plain");
            lua_rawset(L, -3);
        }
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_settop(L, 0); // at rest, as a save wants it
    }

    Unit* make(const char* bp, f32 x, f32 z) {
        REQUIRE(state
                    .do_string("made = CreateUnit('" + std::string(bp) + "', 1, " +
                               std::to_string(x) + ", 10, " + std::to_string(z) + ")")
                    .ok());
        Unit* found = nullptr;
        sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
            const auto& u = static_cast<Unit&>(e);
            if (u.blueprint_id() == bp && u.position().x == x && u.position().z == z)
                found = static_cast<Unit*>(&e);
        });
        REQUIRE(found);
        return found;
    }

    static void move(Unit& u, f32 x, f32 z) {
        osc::sim::UnitCommand cmd;
        cmd.type = osc::sim::CommandType::Move;
        cmd.target_pos = {x, 10.0f, z};
        u.push_command(cmd, true);
    }

    /// Ticks until `u` has no orders left (or `limit`); whether its
    /// footprint always fitted where it stood.
    bool run(Unit& u, int limit, int& ticks) {
        bool fitted = true;
        for (ticks = 0; ticks < limit && !u.command_queue().empty(); ++ticks) {
            sim.tick();
            if (sim.footprint_fits_at(u.footprint(), u.position().x, u.position().z) == 0)
                fitted = false;
        }
        return fitted;
    }
};

} // namespace

TEST_CASE("With Moho pathing a tank's move asks its army, follows the cells and stops in its "
          "goal cell",
          "[moho_pathing]") {
    World w(true);
    Unit& tank = *w.make("tank", 20.5f, 20.5f);
    CHECK(tank.footprint_class() == 0);
    World::move(tank, 100.5f, 90.5f);
    w.sim.tick();
    CHECK(tank.navigator().moho_active());
    int ticks = 0;
    CHECK(w.run(tank, 600, ticks));
    CHECK(tank.command_queue().empty());
    const auto at =
        osc::sim::footprint_rect(tank.footprint(), tank.position().x, tank.position().z);
    CHECK(at.x0 == 100);
    CHECK(at.z0 == 90);
    CHECK_FALSE(tank.navigator().moho_active());
    CHECK(ticks < 400); // ~106 units at 4 a second
}

TEST_CASE("With Moho pathing tanks go round a wall through its gap, never standing in it",
          "[moho_pathing]") {
    for (const char* bp : {"tank", "bigtank"}) {
        INFO(bp);
        World w(true);
        osc::sim::GroundOccupant wall;
        wall.caps = oc::kLand;
        wall.rects = {{60, 0, 62, 70}, {60, 76, 62, static_cast<i32>(kMap)}};
        w.sim.occupy_ground(9999, wall);
        Unit& tank = *w.make(bp, 30.5f, 20.5f);
        World::move(tank, 100.5f, 20.5f);
        int ticks = 0;
        CHECK(w.run(tank, 1200, ticks));
        CHECK(tank.command_queue().empty());
        CHECK(tank.position().x > 95.0f);
        CHECK(std::abs(tank.position().z - 20.5f) < 4.0f);
    }
}

TEST_CASE("With Moho pathing off, a tank moves by the grid pathfinder as before",
          "[moho_pathing]") {
    World w(false);
    Unit& tank = *w.make("tank", 20.5f, 20.5f);
    World::move(tank, 100.5f, 90.5f);
    int ticks = 0;
    w.run(tank, 600, ticks);
    CHECK(tank.command_queue().empty());
    CHECK_FALSE(tank.navigator().moho_active());
    CHECK(std::abs(tank.position().x - 100.5f) < 1.0f);
}

namespace {

/// Where the save test's tank `i` starts: six to a row, twelve apart.
osc::sim::Vector3 start_of(int i) {
    const int col = i % 6;
    const int row = i / 6;
    return {10.5f + static_cast<f32>(col) * 6.0f, 10.0f, 10.5f + static_cast<f32>(row) * 12.0f};
}

} // namespace

TEST_CASE("With Moho pathing a game saved with searches waiting and one in flight loads and "
          "goes on exactly as the original",
          "[moho_pathing]") {
    const auto setup = [](World& w) {
        osc::sim::GroundOccupant wall;
        wall.caps = oc::kLand;
        wall.rects = {{60, 0, 62, 70}, {60, 76, 62, static_cast<i32>(kMap)}};
        w.sim.occupy_ground(9999, wall);
    };
    World a(true);
    setup(a);
    std::vector<Unit*> tanks;
    tanks.reserve(24);
    for (int i = 0; i < 24; ++i)
        tanks.push_back(a.make(i % 3 == 0 ? "bigtank" : "tank", start_of(i).x, start_of(i).z));
    a.sim.set_recording(true);
    a.sim.tick();
    for (size_t i = 0; i < tanks.size(); ++i)
        World::move(*tanks[i], 110.5f - static_cast<f32>(i % 4) * 5.0f,
                    15.5f + static_cast<f32>(i) * 4.0f);
    // A few ticks in: some searches done, one in flight, the rest waiting.
    bool waiting = false;
    for (int t = 0; t < 10 && !waiting; ++t) {
        a.sim.tick();
        const auto& q = a.sim.get_army(0)->path_queue();
        waiting = !q.pending().empty() && q.search().traveler() != nullptr;
    }
    CHECK(waiting);
    const osc::sim::SavedGame save = osc::sim::save_game(a.sim, "paths");
    REQUIRE_FALSE(save.snapshot.empty());

    World b(true);
    setup(b);
    for (int i = 0; i < 24; ++i)
        b.make(i % 3 == 0 ? "bigtank" : "tank", start_of(i).x, start_of(i).z);
    const std::string err = osc::sim::load_snapshot(b.sim, save.snapshot);
    INFO(err);
    REQUIRE(err.empty());
    CHECK(b.sim.moho_pathing());
    CHECK(b.sim.compute_sync_checksum() == a.sim.compute_sync_checksum());
    for (int t = 0; t < 300; ++t) {
        a.sim.tick();
        b.sim.tick();
        INFO("tick " << t);
        REQUIRE(b.sim.compute_sync_checksum() == a.sim.compute_sync_checksum());
    }
    // And every one got through the gap. (Counting the moves that ended
    // would count one given up short of it.)
    int through = 0;
    for (Unit* u : tanks) through += u->position().x > 62.0f ? 1 : 0;
    CHECK(through == 24);
}

TEST_CASE("With Moho pathing a destination the unit won't fit moves to the nearest place it "
          "will (Unit::PrepareMove), so the move ends beside a structure, not round and round",
          "[moho_pathing]") {
    World w(true);
    osc::sim::GroundOccupant site;
    site.caps = oc::kLand;
    site.rects = {{60, 60, 64, 64}};
    w.sim.occupy_ground(9999, site);
    Unit& tank = *w.make("tank", 20.5f, 20.5f);
    World::move(tank, 62.0f, 62.0f); // inside it
    int ticks = 0;
    CHECK(w.run(tank, 600, ticks));
    CHECK(tank.command_queue().empty());
    CHECK(std::abs(tank.position().x - 62.0f) < 5.0f);
    CHECK(std::abs(tank.position().z - 62.0f) < 5.0f);

    // On a cell edge beside it: z = 65.0 rounds to cell 64 (free), where
    // truncating 64.5 would also say 64; z = 64.0 rounds to cell 64 too,
    // where truncating 63.5 says 63, inside.
    Unit& other = *w.make("tank", 30.5f, 80.5f);
    World::move(other, 62.5f, 64.0f);
    CHECK(w.run(other, 600, ticks));
    CHECK(other.command_queue().empty());
    const auto at =
        osc::sim::footprint_rect(other.footprint(), other.position().x, other.position().z);
    CHECK(at.z0 == 64);
    CHECK(ticks < 300);
}

namespace {

/// The checksum's domains that differ between `a` and `b`, by name.
std::string changed_domains(const osc::sim::SimState::ChecksumParts& a,
                            const osc::sim::SimState::ChecksumParts& b) {
    std::string out;
    const auto before = a.values();
    const auto after = b.values();
    for (size_t i = 0; i < before.size(); ++i) {
        if (before[i] == after[i]) continue;
        if (!out.empty()) out += ' ';
        out += osc::sim::SimState::ChecksumParts::kNames[i];
    }
    return out;
}

} // namespace

TEST_CASE("With Moho pathing its state is in the sync checksum's navigation domain, and only "
          "there; with it off, not at all",
          "[moho_pathing][checksum]") {
    for (const bool moho : {true, false}) {
        INFO("moho pathing " << moho);
        World w(moho);
        osc::sim::PathTables& tables = *w.sim.path_tables();
        osc::sim::path::PathQueue& queue = w.sim.get_army(0)->path_queue();
        // Every cluster built first, so dirtying one is news.
        for (int t = 0; t < 2000 && !tables.done(); ++t) w.sim.tick();
        REQUIRE(tables.done());
        const auto check = [&](const char* what, const std::function<void()>& change) {
            const auto before = w.sim.checksum_parts();
            change();
            INFO(what);
            CHECK(changed_domains(before, w.sim.checksum_parts()) == (moho ? "navigation" : ""));
        };
        check("a cluster dirtied", [&] { tables.dirty({40, 40, 44, 44}); });

        // Tanks crossing the wall's gap: searches waiting and one in flight.
        osc::sim::GroundOccupant wall;
        wall.caps = oc::kLand;
        wall.rects = {{60, 0, 62, 70}, {60, 76, 62, static_cast<i32>(kMap)}};
        w.sim.occupy_ground(9999, wall);
        std::vector<Unit*> tanks;
        tanks.reserve(12);
        for (int i = 0; i < 12; ++i)
            tanks.push_back(w.make(i % 3 == 0 ? "bigtank" : "tank", start_of(i).x, start_of(i).z));
        for (size_t i = 0; i < tanks.size(); ++i)
            World::move(*tanks[i], 110.5f, 15.5f + static_cast<f32>(i) * 4.0f);
        // Ticks until their searches wait (the queue works them as the
        // next tick begins).
        for (int t = 0; t < 10 && queue.pending().empty(); ++t) w.sim.tick();
        if (moho) REQUIRE(queue.pending().size() > 2);
        if (moho) {
            const auto work_once = [&] {
                osc::i32 budget = 1;
                queue.work(tables, budget);
            };
            check("a search begun", work_once);
            REQUIRE(queue.search().traveler());
            check("its progress", work_once);
            check("a search cancelled", [&] { queue.cancel(*queue.pending().back()); });
        }
        // Then one following its path.
        Unit* moving = nullptr;
        for (int t = 0; t < 20 && !moving; ++t) {
            w.sim.tick();
            for (Unit* u : tanks)
                if (!moving && !queue.queued(u->navigator().moho_path().finder()) &&
                    u->navigator().moho_path().state() ==
                        osc::sim::path::PathNavigator::State::HasPath)
                    moving = u;
        }
        if (moho) REQUIRE(moving);
        if (!moving) moving = tanks.front();
        osc::sim::path::PathNavigator& nav = moving->navigator().moho_path();
        check("a search queued", [&] { queue.queue(nav.finder()); });
        check("the next search's type",
              [&] { nav.finder().set_type(osc::sim::path::SearchType::Leader); });
        check("a repath asked", [&] { nav.request_repath(); });
        check("whether it moved last tick", [&] { moving->note_tick_position(); });
        check("a new goal", [&] {
            nav.set_goal({{100, 100, 104, 104}, {}}, moving->position().x, moving->position().z);
        });
        check("its path dropped", [&] { nav.reset(); });
    }
}

// Roadmap P1: the cases a default-on switch must survive. Each runs on the
// flat map with walls of occupied ground, and checks no unit ever stands
// where its footprint doesn't fit.

namespace {

/// Ticks until none of `units` has orders left (or `limit`); whether every
/// footprint always fitted where it stood.
bool run_all(World& w, const std::vector<Unit*>& units, int limit, int& ticks) {
    bool fitted = true;
    const auto busy = [&] {
        for (const Unit* u : units)
            if (!u->command_queue().empty()) return true;
        return false;
    };
    for (ticks = 0; ticks < limit && busy(); ++ticks) {
        w.sim.tick();
        for (const Unit* u : units)
            if (w.sim.footprint_fits_at(u->footprint(), u->position().x, u->position().z) == 0)
                fitted = false;
    }
    return fitted;
}

/// The closest two of `units` stand.
f32 closest_pair(const std::vector<Unit*>& units) {
    f32 best = 1e9f;
    for (size_t i = 0; i < units.size(); ++i)
        for (size_t j = i + 1; j < units.size(); ++j) {
            const f32 dx = units[i]->position().x - units[j]->position().x;
            const f32 dz = units[i]->position().z - units[j]->position().z;
            best = std::min(best, std::sqrt(dx * dx + dz * dz));
        }
    return best;
}

/// Spot `i` of a grid `per_row` wide from (x0, z0), `step_x` and `step_z` apart.
osc::sim::Vector3 grid_spot(int i, int per_row, f32 x0, f32 z0, f32 step_x, f32 step_z) {
    const int col = i % per_row;
    const int row = i / per_row;
    return {x0 + static_cast<f32>(col) * step_x, 10.0f, z0 + static_cast<f32>(row) * step_z};
}

/// A wall of occupied ground down x = 60..62, open at z = [gap0, gap1).
void wall_with_gap(World& w, i32 gap0, i32 gap1) {
    osc::sim::GroundOccupant wall;
    wall.caps = oc::kLand;
    wall.rects = {{60, 0, 62, gap0}, {60, gap1, 62, static_cast<i32>(kMap)}};
    w.sim.occupy_ground(9999, wall);
}

} // namespace

TEST_CASE("With Moho pathing a gap two cells wide lets a 1x1 tank through and not a 3x3 one, "
          "which gives up on the near side",
          "[moho_pathing][scenarios]") {
    World w(true);
    wall_with_gap(w, 70, 72);
    Unit& small = *w.make("tank", 30.5f, 70.5f);
    Unit& big = *w.make("bigtank", 30.5f, 40.5f);
    World::move(small, 100.5f, 70.5f);
    World::move(big, 100.5f, 70.5f);
    int ticks = 0;
    CHECK(run_all(w, {&small, &big}, 1500, ticks));
    INFO("ticks " << ticks);
    CHECK(small.command_queue().empty());
    CHECK(small.position().x > 99.0f);
    CHECK(big.command_queue().empty());
    CHECK(big.position().x < 60.0f); // never through
    CHECK(big.position().x > 50.0f); // but as near as it fits
    // Given up: no search of its still waiting or in flight.
    const auto& queue = w.sim.get_army(0)->path_queue();
    CHECK(queue.pending().empty());
    CHECK(queue.search().traveler() == nullptr);
}

TEST_CASE("With Moho pathing a goal walled in all round ends the move outside, beside it, "
          "without searching on",
          "[moho_pathing][scenarios]") {
    World w(true);
    osc::sim::GroundOccupant box;
    box.caps = oc::kLand;
    box.rects = {{90, 90, 110, 92}, {90, 108, 110, 110}, {90, 92, 92, 108}, {108, 92, 110, 108}};
    w.sim.occupy_ground(9999, box);
    Unit& tank = *w.make("tank", 30.5f, 30.5f);
    World::move(tank, 100.5f, 100.5f);
    int ticks = 0;
    CHECK(w.run(tank, 1500, ticks));
    INFO("ticks " << ticks);
    CHECK(tank.command_queue().empty());
    const f32 dx = tank.position().x - 100.5f;
    const f32 dz = tank.position().z - 100.5f;
    CHECK(std::sqrt(dx * dx + dz * dz) < 16.0f); // at the box's wall
    const bool inside = tank.position().x > 90.0f && tank.position().x < 110.0f &&
                        tank.position().z > 90.0f && tank.position().z < 110.0f;
    CHECK_FALSE(inside);
    const auto& queue = w.sim.get_army(0)->path_queue();
    CHECK(queue.pending().empty());
    CHECK(queue.search().traveler() == nullptr);
    // And nothing more asked for it: a tick on, the army's queue stays idle.
    for (int t = 0; t < 30; ++t) w.sim.tick();
    CHECK(queue.pending().empty());
    CHECK(queue.search().traveler() == nullptr);
}

TEST_CASE("With Moho pathing a crowd of twenty goes through one gap to twenty places beyond "
          "it, and every one arrives",
          "[moho_pathing][scenarios]") {
    World w(true);
    wall_with_gap(w, 70, 76);
    std::vector<Unit*> tanks;
    tanks.reserve(20);
    for (int i = 0; i < 20; ++i) {
        const osc::sim::Vector3 at = grid_spot(i, 5, 30.5f, 60.5f, 3.0f, 6.0f);
        tanks.push_back(w.make("tank", at.x, at.z));
    }
    for (int i = 0; i < 20; ++i) {
        const osc::sim::Vector3 to = grid_spot(i, 5, 90.5f, 60.5f, 3.0f, 6.0f);
        World::move(*tanks[static_cast<size_t>(i)], to.x, to.z);
    }
    int ticks = 0;
    CHECK(run_all(w, tanks, 3000, ticks));
    INFO("ticks " << ticks);
    int arrived = 0;
    for (const Unit* u : tanks) arrived += u->command_queue().empty() && u->position().x > 85.0f;
    CHECK(arrived == 20);
    CHECK(closest_pair(tanks) > 1.0f); // none stacked on another
}

TEST_CASE("With Moho pathing a formation of nine goes through the gap to its slots, and a game "
          "saved on the way goes on as the original",
          "[moho_pathing][scenarios]") {
    // Nine slots of one formation order (UnitCommand::formed, one command
    // id: one formation layer), three apart.
    const auto scene = [](World& w) {
        wall_with_gap(w, 70, 76);
        std::vector<Unit*> tanks;
        tanks.reserve(9);
        for (int i = 0; i < 9; ++i) {
            const osc::sim::Vector3 at = grid_spot(i, 3, 30.5f, 64.5f, 3.0f, 3.0f);
            tanks.push_back(w.make("tank", at.x, at.z));
        }
        return tanks;
    };
    const auto slot = [](size_t i) {
        return grid_spot(static_cast<int>(i), 3, 95.5f, 70.5f, 3.0f, 3.0f);
    };
    World a(true);
    const std::vector<Unit*> tanks = scene(a);
    a.sim.set_recording(true);
    a.sim.tick();
    for (size_t i = 0; i < tanks.size(); ++i) {
        osc::sim::UnitCommand cmd;
        cmd.type = osc::sim::CommandType::Move;
        cmd.target_pos = slot(i);
        cmd.command_id = 77;
        cmd.formation = "AttackFormation";
        cmd.formed = true;
        cmd.speed_cap = 4.0f;
        tanks[i]->push_command(cmd, true);
    }
    for (int t = 0; t < 40; ++t) a.sim.tick();
    const osc::sim::SavedGame save = osc::sim::save_game(a.sim, "formation");
    REQUIRE_FALSE(save.snapshot.empty());

    World b(true);
    scene(b);
    const std::string err = osc::sim::load_snapshot(b.sim, save.snapshot);
    INFO(err);
    REQUIRE(err.empty());
    CHECK(b.sim.compute_sync_checksum() == a.sim.compute_sync_checksum());
    bool fitted = true;
    for (int t = 0; t < 600; ++t) {
        a.sim.tick();
        b.sim.tick();
        INFO("tick " << t);
        REQUIRE(b.sim.compute_sync_checksum() == a.sim.compute_sync_checksum());
        for (const Unit* u : tanks)
            if (a.sim.footprint_fits_at(u->footprint(), u->position().x, u->position().z) == 0)
                fitted = false;
    }
    CHECK(fitted);
    for (size_t i = 0; i < tanks.size(); ++i) {
        INFO("slot " << i);
        CHECK(tanks[i]->command_queue().empty());
        const auto at = osc::sim::footprint_rect(tanks[i]->footprint(), tanks[i]->position().x,
                                                 tanks[i]->position().z);
        const auto want = osc::sim::footprint_rect(tanks[i]->footprint(), slot(i).x, slot(i).z);
        CHECK(at.x0 == want.x0);
        CHECK(at.z0 == want.z0);
    }
}
