// Units moving as Moho's do (roadmap item 4c-2c): with the sim's switch on,
// a ground unit's move asks its army's path queue, and its navigator
// follows the cells (faf-re CAiNavigatorLand, CAiPathNavigator), driving at
// each target and refused ground it won't fit.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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
#include "support/tick_until.hpp"

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using osc::f32;
using osc::i32;
using osc::u32;
using osc::u8;
using osc::blueprints::NamedFootprint;
using osc::sim::EntityId;
using osc::sim::Unit;
using osc::test::tick_until;
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
                 " Defense = {MaxHealth = 100}, SizeX = 0.7, SizeY = 0.5, SizeZ = 0.9,"
                 " Footprint = {SizeX = 1, SizeZ = 1},"
                 " Physics = {MotionType = 'RULEUMT_Land', MaxSpeed = 4, MaxAcceleration = 4,"
                 "  MaxBrake = 4, TurnRate = 180, TurnRadius = 0}}",
                 "{BlueprintId = 'bigtank', Categories = {'LAND', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, Footprint = {SizeX = 3, SizeZ = 3},"
                 " Physics = {MotionType = 'RULEUMT_Land', MaxSpeed = 4, MaxAcceleration = 4,"
                 "  MaxBrake = 4, TurnRate = 180, TurnRadius = 0}}",
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

    /// Ticks until `u` has no orders left, failing at `limit`; whether its
    /// footprint always fitted where it stood.
    bool run(Unit& u, std::string_view what, int limit, int& ticks) {
        bool fitted = true;
        ticks = tick_until(
            [&] {
                sim.tick();
                if (sim.footprint_fits_at(u.footprint(), u.position().x, u.position().z) == 0) {
                    fitted = false;
                }
            },
            what, limit, [&] { return u.command_queue().empty(); });
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
    CHECK(w.run(tank, "the tank's move ends", 600, ticks));
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
        w.sim.occupy_ground(EntityId::of(9999, 1), wall);
        Unit& tank = *w.make(bp, 30.5f, 20.5f);
        World::move(tank, 100.5f, 20.5f);
        int ticks = 0;
        CHECK(w.run(tank, "the tank's move round the wall ends", 1200, ticks));
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
    w.run(tank, "the tank's move ends", 600, ticks);
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
        w.sim.occupy_ground(EntityId::of(9999, 1), wall);
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
    tick_until([&] { a.sim.tick(); }, "searches wait and one is in flight", 10,
               [&] {
                   const auto& q = a.sim.get_army(0)->path_queue();
                   return !q.pending().empty() && q.search().traveler() != nullptr;
               });
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
    // Moho's motion pushes units that overlap apart (Sim::DoCollisionsFor), so the jam at
    // the gap takes longer to clear than the engine's old separation did.
    for (int t = 0; t < 600; ++t) {
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
    w.sim.occupy_ground(EntityId::of(9999, 1), site);
    Unit& tank = *w.make("tank", 20.5f, 20.5f);
    World::move(tank, 62.0f, 62.0f); // inside it
    int ticks = 0;
    CHECK(w.run(tank, "the move into the structure ends", 600, ticks));
    CHECK(std::abs(tank.position().x - 62.0f) < 5.0f);
    CHECK(std::abs(tank.position().z - 62.0f) < 5.0f);

    // On a cell edge beside it: z = 65.0 rounds to cell 64 (free), where
    // truncating 64.5 would also say 64; z = 64.0 rounds to cell 64 too,
    // where truncating 63.5 says 63, inside.
    Unit& other = *w.make("tank", 30.5f, 80.5f);
    World::move(other, 62.5f, 64.0f);
    CHECK(w.run(other, "the move beside the structure ends", 600, ticks));
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
        tick_until([&] { w.sim.tick(); }, "every cluster is built", 2000,
                   [&] { return tables.done(); });
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
        w.sim.occupy_ground(EntityId::of(9999, 1), wall);
        std::vector<Unit*> tanks;
        tanks.reserve(24);
        for (int i = 0; i < 24; ++i) {
            tanks.push_back(w.make(i % 3 == 0 ? "bigtank" : "tank", start_of(i).x, start_of(i).z));
        }
        for (size_t i = 0; i < tanks.size(); ++i)
            World::move(*tanks[i], 110.5f, 15.5f + static_cast<f32>(i) * 4.0f);
        if (moho) {
            tick_until([&] { w.sim.tick(); }, "searches past its army's budget wait", 10,
                       [&] { return !queue.pending().empty(); });
            REQUIRE(queue.pending().size() > 2);
            const auto work_once = [&] {
                osc::i32 budget = 1;
                queue.work(tables, budget);
            };
            check("a search begun", work_once);
            REQUIRE(queue.search().traveler());
            check("its progress", work_once);
            check("a search cancelled", [&] { queue.cancel(*queue.pending().back()); });
        }
        Unit* moving = nullptr;
        if (moho) {
            tick_until([&] { w.sim.tick(); }, "a tank follows its path", 20,
                       [&] {
                           for (Unit* u : tanks) {
                               if (!queue.queued(u->navigator().moho_path().finder()) &&
                                   u->navigator().moho_path().state() ==
                                       osc::sim::path::PathNavigator::State::HasPath) {
                                   moving = u;
                                   return true;
                               }
                           }
                           return false;
                       });
        }
        if (!moving) moving = tanks.front();
        osc::sim::path::PathNavigator& nav = moving->navigator().moho_path();
        check("a search queued", [&] { queue.queue(nav.finder()); });
        check("the next search's type",
              [&] { nav.finder().set_type(osc::sim::path::SearchType::Leader); });
        check("a repath asked", [&] { nav.request_repath(); });
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

/// Ticks until none of `units` has orders left, failing at `limit`; whether every
/// footprint always fitted where it stood.
bool run_all(World& w, const std::vector<Unit*>& units, std::string_view what, int limit,
             int& ticks) {
    bool fitted = true;
    const auto busy = [&] {
        for (const Unit* u : units)
            if (!u->command_queue().empty()) return true;
        return false;
    };
    ticks = tick_until(
        [&] {
            w.sim.tick();
            for (const Unit* u : units) {
                if (w.sim.footprint_fits_at(u->footprint(), u->position().x, u->position().z) ==
                    0) {
                    fitted = false;
                }
            }
        },
        what, limit, [&] { return !busy(); });
    return fitted;
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
    w.sim.occupy_ground(EntityId::of(9999, 1), wall);
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
    CHECK(run_all(w, {&small, &big}, "both tanks' moves end", 1500, ticks));
    INFO("ticks " << ticks);
    CHECK(small.position().x > 99.0f);
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
    w.sim.occupy_ground(EntityId::of(9999, 1), box);
    Unit& tank = *w.make("tank", 30.5f, 30.5f);
    World::move(tank, 100.5f, 100.5f);
    w.sim.tick();
    CHECK(tank.navigator().moho_active());
    int ticks = 0;
    CHECK(w.run(tank, "the move to the walled-in goal ends", 1500, ticks));
    INFO("ticks " << ticks);
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

TEST_CASE("With Moho pathing a tank wedged at a wall's end stands still and gives its move up",
          "[moho_pathing][scenarios]") {
    // Retail (tools/moho_probes/corner): a Striker at the end of a wall two cells thick,
    // inside its width, ordered to a goal whose line clips the wall's end, moves 0.02 and
    // stands still; its order ends on tick 40. A step that won't fit leaves it where
    // it was (CalcMoveCommon), so its navigator sees it still and gives the move up after
    // 30 ticks. (A crowd through a gap is data.crowdgap-test, on retail's units.)
    World w(true);
    osc::sim::GroundOccupant wall;
    wall.caps = oc::kLand;
    wall.rects = {{60, 76, 62, static_cast<i32>(kMap)}};
    w.sim.occupy_ground(EntityId::of(9999, 1), wall);
    Unit& tank = *w.make("tank", 61.77f, 75.998f);
    World::move(tank, 90.5f, 78.5f);
    osc::sim::Vector3 last = tank.position();
    f32 moved_late = 0;
    int ended = -1;
    for (int t = 0; t < 120; ++t) {
        w.sim.tick();
        const osc::sim::Vector3 at = tank.position();
        if (t >= 10) moved_late += std::abs(at.x - last.x) + std::abs(at.z - last.z);
        last = at;
        if (ended < 0 && tank.command_queue().empty()) ended = t;
    }
    INFO("ended on tick " << ended << ", moved " << moved_late << " after tick 10, at "
                          << tank.position().x << ", " << tank.position().z);
    CHECK(moved_late < 0.05f);           // still, not rocking
    CHECK((ended >= 30 && ended <= 60)); // given up after 30 still ticks
    CHECK(tank.position().x < 62.5f);    // at the wall's end
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

namespace {

void queue_move(Unit& u, f32 x, f32 z) {
    osc::sim::UnitCommand cmd;
    cmd.type = osc::sim::CommandType::Move;
    cmd.target_pos = {x, 10.0f, z};
    u.push_command(cmd, false);
}

f32 step_of(World& w, const Unit& u) {
    const osc::sim::Vector3 was = u.position();
    w.sim.tick();
    const f32 dx = u.position().x - was.x;
    const f32 dz = u.position().z - was.z;
    return std::sqrt(dx * dx + dz * dz);
}

i32 cell_z(const Unit& u) {
    return osc::sim::footprint_rect(u.footprint(), u.position().x, u.position().z).z0;
}

/// The thread runs in the next tick's Lua stage: that tick is T.
void move_from_thread(World& w, Unit& u) {
    u.add_command_cap("RULEUCC_Move");
    REQUIRE(w.state.do_string("function go() IssueMove({made}, {20.5, 10, 60.5}) end").ok());
    REQUIRE(w.state.do_string("ForkThread(go)").ok());
}

} // namespace

TEST_CASE("With Moho pathing an IssueMove from a Lua thread at tick T first steps along its path "
          "at T+3",
          "[moho_pathing][tick-phases]") {
    World w(true);
    Unit& tank = *w.make("tank", 20.5f, 20.5f);
    move_from_thread(w, tank);
    for (int t = 0; t < 3; ++t) {
        INFO("T+" << t);
        CHECK(step_of(w, tank) == 0.0f);
    }
    CHECK(step_of(w, tank) > 0.0f);
    CHECK(tank.position().z > 20.5f);
}

TEST_CASE("With Moho pathing off, an IssueMove from a Lua thread at tick T first steps at T+1",
          "[moho_pathing][tick-phases]") {
    World w(false);
    Unit& tank = *w.make("tank", 20.5f, 20.5f);
    move_from_thread(w, tank);
    CHECK(step_of(w, tank) == 0.0f);
    CHECK(step_of(w, tank) > 0.0f);
    CHECK(tank.position().z > 20.5f);
}

TEST_CASE("With Moho pathing a two-point move chain slows for two ticks at the first point without "
          "stopping",
          "[moho_pathing][tick-phases]") {
    World w(true);
    Unit& tank = *w.make("tank", 20.5f, 20.5f);
    queue_move(tank, 20.5f, 40.5f);
    queue_move(tank, 20.5f, 70.5f);
    f32 into = 0.0f;
    tick_until([&] { into = step_of(w, tank); }, "the tank reaches the first point's cell", 300,
               [&] { return cell_z(tank) == 40; });
    REQUIRE(tank.command_queue().size() == 2);
    const f32 first = step_of(w, tank);
    CHECK(tank.command_queue().size() == 1);
    const f32 second = step_of(w, tank);
    const f32 on = step_of(w, tank);
    CHECK(first < into);
    CHECK(second < first);
    CHECK(second > 0.0f);
    CHECK(on > second);
}

TEST_CASE("With Moho pathing a unit walled in where it stands gives its move up four ticks "
          "after it is set, never moving",
          "[moho_pathing][tick-phases]") {
    World w(true);
    osc::sim::GroundOccupant box;
    box.caps = oc::kLand;
    box.rects = {{19, 19, 22, 20}, {19, 21, 22, 22}, {19, 20, 20, 21}, {21, 20, 22, 21}};
    w.sim.occupy_ground(EntityId::of(9999, 1), box);
    Unit& tank = *w.make("tank", 20.5f, 20.5f);
    World::move(tank, 60.5f, 60.5f);
    for (int t = 1; t <= 4; ++t) {
        INFO("tick " << t);
        CHECK(step_of(w, tank) == 0.0f);
        CHECK(tank.command_queue().size() == 1);
    }
    CHECK(tank.navigator().moho_path().state() == osc::sim::path::PathNavigator::State::Failed);
    CHECK(step_of(w, tank) == 0.0f);
    CHECK(tank.command_queue().empty());
}

namespace {

void patrol(Unit& u, osc::sim::CommandType type, f32 x, f32 z, bool clear) {
    osc::sim::UnitCommand cmd;
    cmd.type = type;
    cmd.target_pos = {x, 10.0f, z};
    u.push_command(cmd, clear);
}

} // namespace

TEST_CASE("A patrol leg ends on a run of its task within the unit's top speed in cells of its "
          "point",
          "[patrol][pathing]") {
    const bool moho = GENERATE(false, true);
    const int points = GENERATE(1, 2);
    INFO("moho " << moho << ", points " << points);
    World w(moho);
    Unit* tank = w.make("tank", 20.5f, 30.5f);
    patrol(*tank, osc::sim::CommandType::Patrol, 60.5f, 30.5f, true);
    if (points == 2) {
        patrol(*tank, osc::sim::CommandType::Patrol, 20.5f, 30.5f, false);
    }
    tick_until([&] { w.sim.tick(); }, "the first leg ends", 400,
               [&] {
                   return tank->command_queue().empty() ||
                          tank->command_queue().front().target_pos.x != 60.5f;
               });
    const f32 short_of = 60.5f - tank->position().x;
    CHECK(short_of > 1.0f);
    CHECK(short_of <= 4.5f);
    if (points == 1) {
        CHECK(tank->command_queue().empty());
    } else {
        REQUIRE(tank->command_queue().size() == 2);
        CHECK(tank->command_queue().back().target_pos.x == 60.5f);
    }
}

TEST_CASE("A patrol leg lasts to its task's fourth run", "[patrol][pathing]") {
    const bool moho = GENERATE(false, true);
    INFO("moho " << moho);
    World w(moho);
    Unit* tank = w.make("tank", 20.5f, 30.5f);
    patrol(*tank, osc::sim::CommandType::Patrol, 22.5f, 30.5f, true);
    for (int t = 0; t < 18; ++t) {
        w.sim.tick();
    }
    CHECK_FALSE(tank->command_queue().empty());
    w.sim.tick();
    CHECK(tank->command_queue().empty());
}

TEST_CASE("An attack-move ends as its patrol task's leg does, short of its point",
          "[patrol][attack-move][pathing]") {
    const bool moho = GENERATE(false, true);
    INFO("moho " << moho);
    World w(moho);
    Unit* tank = w.make("tank", 20.5f, 30.5f);
    patrol(*tank, osc::sim::CommandType::AggressiveMove, 60.5f, 30.5f, true);
    int ticks = 0;
    w.run(*tank, "the attack-move ends", 400, ticks);
    const f32 short_of = 60.5f - tank->position().x;
    CHECK(short_of > 1.0f);
    CHECK(short_of <= 4.5f);
}

TEST_CASE("With Moho pathing a patrol leg's end brakes its unit along its spline",
          "[patrol][pathing]") {
    World w(true);
    Unit* tank = w.make("tank", 20.5f, 30.5f);
    patrol(*tank, osc::sim::CommandType::Patrol, 60.5f, 30.5f, true);
    int ticks = 0;
    w.run(*tank, "the patrol ends", 400, ticks);
    const f32 ended = tank->position().x;
    for (int t = 0; t < 30; ++t) {
        w.sim.tick();
    }
    CHECK(tank->position().x - ended > 1.2f);
}

TEST_CASE("With Moho pathing a moving tank whose orders are cleared brakes along its spline",
          "[pathing]") {
    World w(true);
    Unit* tank = w.make("tank", 20.5f, 30.5f);
    World::move(*tank, 60.5f, 30.5f);
    for (int t = 0; t < 60; ++t) {
        w.sim.tick();
    }
    REQUIRE(tank->navigator().busy());
    const f32 cleared = tank->position().x;
    tank->clear_commands();
    for (int t = 0; t < 30; ++t) {
        w.sim.tick();
    }
    CHECK(tank->position().x - cleared > 1.2f);
}
