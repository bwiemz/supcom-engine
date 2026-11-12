// The default pathing's reference games. Each plays a scene on the default
// pathing (Moho's: footprint classes, its path queue, spline and steering)
// on one synthetic map, checks what its units must do, and pins the
// checksum it ends on. CI builds it on GCC, Clang, ASan, MSVC and macOS, so
// each pinned value is a cross-platform lockstep check of the default
// pathing in that kind of play; test_cross_platform.cpp's synthetic game
// has no map, so its units never take the default pathing at all.
//
// Each scene is also saved in the middle of its motion, loaded into a fresh
// sim and played on beside the original: the two agree on every beat and
// end on the same pinned value.
//
// A pinned value changes when a rule of the pathing, the motion or the
// sim's arithmetic does: update it on purpose, from a run on any one
// platform, and say why.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "blueprints/footprint.hpp"
#include "lua/blueprint_bindings.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
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
using osc::sim::EntityId;
using osc::sim::Unit;
namespace oc = osc::blueprints::occupancy;

namespace {

constexpr u32 kMap = 128;
constexpr f32 kWaterLevel = 8.0f;
constexpr f32 kLandHeight = 10.0f;
constexpr f32 kBedHeight = 3.0f;

/// The map, west to east: land at 10 to x = 60; a shore down to the lake's
/// bed at 3 by x = 74, 5 under the water; an island at 10 from x = 86 to
/// 94, z = 52 to 76; the bed again to x = 106; a shore up to land at 10 by
/// x = 118, and land to the east edge. Slopes on the shores are 0.5 to
/// 0.6, under the land classes' 0.75.
f32 ground_at(u32 x, u32 z) {
    const auto lerp = [](f32 from, f32 to, f32 t) { return from + (to - from) * t; };
    if (x >= 86 && x <= 94 && z >= 52 && z <= 76) return kLandHeight;
    if (x <= 60) return kLandHeight;
    if (x < 74) return lerp(kLandHeight, kBedHeight, static_cast<f32>(x - 60) / 14.0f);
    if (x <= 106) return kBedHeight;
    if (x < 118) return lerp(kBedHeight, kLandHeight, static_cast<f32>(x - 106) / 12.0f);
    return kLandHeight;
}

std::unique_ptr<osc::map::Terrain> lake_terrain() {
    std::vector<osc::u16> heights(static_cast<size_t>(kMap + 1) * (kMap + 1));
    for (u32 z = 0; z <= kMap; ++z) {
        for (u32 x = 0; x <= kMap; ++x) {
            heights[static_cast<size_t>(z) * (kMap + 1) + x] =
                static_cast<osc::u16>(std::lround(ground_at(x, z) * 128.0f));
        }
    }
    osc::map::Heightmap hm(kMap, kMap, 1.0f / 128.0f, std::move(heights));
    return std::make_unique<osc::map::Terrain>(std::move(hm), kWaterLevel, true);
}

// Retail's /lua/footprints.lua, as its loader runs it (test_footprint_classes.cpp).
const char* const kRetailFootprints = R"(
LAND = 0x01
SEABED = 0x02
SUB = 0x04
WATER = 0x08
AIR = 0x10
ORBIT = 0x20
IgnoreStructures = 0x01
SpecFootprints {
    { Name = 'Vehicle1x1',   SizeX=1,  SizeZ=1,  Caps=LAND, MaxWaterDepth=0.05, MaxSlope=0.75, Flags=0 },
    { Name = 'Vehicle2x2',   SizeX=2,  SizeZ=2,  Caps=LAND, MaxWaterDepth=0.05, MaxSlope=0.75, Flags=0 },
    { Name = 'Vehicle5x5',   SizeX=5,  SizeZ=5,  Caps=LAND, MaxWaterDepth=0.05, MaxSlope=0.75, Flags=IgnoreStructures },
    { Name = 'Amphibious1x1',   SizeX=1,  SizeZ=1,  Caps=LAND|SEABED, MaxWaterDepth=25, MaxSlope=0.75, Flags=0 },
    { Name = 'Amphibious3x3',   SizeX=3,  SizeZ=3,  Caps=LAND|SEABED, MaxWaterDepth=25, MaxSlope=0.75, Flags=IgnoreStructures },
    { Name = 'Amphibious6x6',   SizeX=6,  SizeZ=6,  Caps=LAND|SEABED, MaxWaterDepth=25, MaxSlope=0.75, Flags=IgnoreStructures },
    { Name = 'WaterLand1x1',   SizeX=1,  SizeZ=1,  Caps=LAND|WATER, MaxWaterDepth=1, MinWaterDepth=0.1, MaxSlope=0.75, Flags=0 },
    { Name = 'WaterLand2x2',   SizeX=2,  SizeZ=2,  Caps=LAND|WATER, MaxWaterDepth=1, MinWaterDepth=0.1, MaxSlope=0.75, Flags=0 },
    { Name = 'WaterLand3x3',   SizeX=3,  SizeZ=3,  Caps=LAND|WATER, MaxWaterDepth=5, MinWaterDepth=0, MaxSlope=0.75, Flags=0 },
    { Name = 'WaterLand5x5',   SizeX=5,  SizeZ=5,  Caps=LAND|WATER, MaxWaterDepth=5, MinWaterDepth=0, MaxSlope=0.75, Flags=0 },
    { Name = 'SurfacingSub2x2',   SizeX=2,  SizeZ=2,  Caps=SUB|WATER, MinWaterDepth=1.5, Flags=0 },
    { Name = 'SurfacingSub3x3',   SizeX=3,  SizeZ=3,  Caps=SUB|WATER, MinWaterDepth=1.5, Flags=0 },
    { Name = 'SurfacingSub4x4',   SizeX=4,  SizeZ=4,  Caps=SUB|WATER, MinWaterDepth=1.5, Flags=0 },
    { Name = 'SurfacingSub12x12', SizeX=12, SizeZ=12, Caps=SUB|WATER, MinWaterDepth=1.5, Flags=0 },
    { Name = 'Water1x1',   SizeX=1,  SizeZ=1,  Caps=WATER, MinWaterDepth=1.5, Flags=0 },
    { Name = 'Water3x3',   SizeX=3,  SizeZ=3,  Caps=WATER, MinWaterDepth=0.25, Flags=0 },
    { Name = 'Water4x4',   SizeX=4,  SizeZ=4,  Caps=WATER, MinWaterDepth=1.5, Flags=0 },
    { Name = 'Water6x6',   SizeX=6,  SizeZ=6,  Caps=WATER, MinWaterDepth=1.5, Flags=0 },
    { Name = 'Water8x8',   SizeX=8,  SizeZ=8,  Caps=WATER, MinWaterDepth=1.5, Flags=0 },
    { Name = 'Water11x11', SizeX=11, SizeZ=11, Caps=WATER, MinWaterDepth=1.5, Flags=0 },
}
)";

std::string unit_blueprint(const char* id, const char* motion, int size, f32 speed) {
    return std::string("{BlueprintId = '") + id + "', Categories = {'MOBILE'}," +
           " Defense = {MaxHealth = 100}, SizeX = " +
           std::to_string(0.7f * static_cast<f32>(size)) +
           ", SizeY = 0.5, SizeZ = " + std::to_string(0.9f * static_cast<f32>(size)) +
           ", Footprint = {SizeX = " + std::to_string(size) + ", SizeZ = " + std::to_string(size) +
           "}, Physics = {MotionType = '" + motion + "', MaxSpeed = " + std::to_string(speed) +
           ", MaxAcceleration = 4, MaxBrake = 4, TurnRate = 180, TurnRadius = 0}}";
}

/// The wall west of the lake, with its gap: x 28 to 30, open from z = 58 to 64.
constexpr i32 kWallX0 = 28;
constexpr i32 kWallX1 = 30;
constexpr i32 kGapZ0 = 58;
constexpr i32 kGapZ1 = 64;

/// One game on the reference map, booted alike for a scene's first play and
/// for the sim a save of it loads into. `units` (Lua) and `ids` hold the
/// units a scene made, in the order it made them.
struct Game {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};
    std::vector<EntityId> ids;

    Game() {
        lua_State* L = state.raw();
        state.set_blueprint_store(&store);
        osc::lua::register_blueprint_store_bindings(state);
        REQUIRE(state.do_string(kRetailFootprints).ok());
        osc::sim::GameSetup game;
        game.scenario = "/maps/test/test_scenario.lua";
        game.seed = 11;
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        sim.set_terrain(lake_terrain());
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        sim.set_game_setup(game);
        sim.set_victory_condition("sandbox"); // no commanders: nobody is eliminated
        osc::sim::GroundOccupant wall;
        wall.caps = oc::kLand;
        wall.rects = {{kWallX0, 0, kWallX1, kGapZ0}, {kWallX0, kGapZ1, kWallX1, 100}};
        sim.occupy_ground(EntityId::of(9999, 1), wall);
        const std::vector<std::string> blueprints = {
            unit_blueprint("tank", "RULEUMT_Land", 1, 4.0f),
            unit_blueprint("bigtank", "RULEUMT_Land", 3, 3.0f),
            unit_blueprint("frigate", "RULEUMT_Water", 3, 5.0f),
            unit_blueprint("sub", "RULEUMT_SurfacingSub", 2, 4.0f),
            unit_blueprint("hover", "RULEUMT_Hover", 1, 4.0f),
            unit_blueprint("amphib", "RULEUMT_Amphibious", 1, 3.0f),
        };
        for (const std::string& bp : blueprints) {
            REQUIRE(state.do_string("return " + bp).ok());
            store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
            lua_pop(L, 1);
        }
        store.expose_to_lua(L);
        REQUIRE(state
                    .do_string("Plain = setmetatable({}, {__index = moho.unit_methods})"
                               " Plain.__index = Plain units = {}")
                    .ok());
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        for (const char* id : {"tank", "bigtank", "frigate", "sub", "hover", "amphib"}) {
            lua_pushstring(L, id);
            lua_getglobal(L, "Plain");
            lua_rawset(L, -3);
        }
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_settop(L, 0); // at rest, as a save wants it
        sim.set_recording(true);
    }

    /// Make a unit of `bp` for `army` (1-based) at (x, z), as `units[n]` in
    /// Lua, n its index in `ids`.
    EntityId make(const char* bp, int army, f32 x, f32 z) {
        const size_t n = ids.size();
        lua("units[" + std::to_string(n + 1) + "] = CreateUnit('" + bp + "', " +
            std::to_string(army) + ", " + std::to_string(x) + ", 10, " + std::to_string(z) + ")");
        EntityId found{};
        sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
            const auto& u = static_cast<Unit&>(e);
            if (u.blueprint_id() == bp && u.position().x == x && u.position().z == z &&
                std::find(ids.begin(), ids.end(), e.entity_id()) == ids.end()) {
                found = e.entity_id();
            }
        });
        REQUIRE(found != EntityId{});
        ids.push_back(found);
        return found;
    }

    /// The scene's unit `n`, or null once it is gone.
    Unit* unit(size_t n) const { return static_cast<Unit*>(sim.entity_registry().find(ids.at(n))); }

    /// The scene's unit `n`, which must still be there.
    Unit& at(size_t n) const {
        Unit* u = unit(n);
        INFO("unit " << n);
        REQUIRE(u);
        return *u;
    }

    void lua(const std::string& code) {
        const auto done = state.do_string(code);
        INFO(code << ": " << (done.ok() ? std::string() : done.error().message));
        REQUIRE(done.ok());
    }

    void move(size_t n, f32 x, f32 z) const {
        osc::sim::UnitCommand cmd;
        cmd.type = osc::sim::CommandType::Move;
        cmd.target_pos = {x, 10.0f, z};
        at(n).push_command(cmd, true);
    }

    f32 ground(const Unit& u) const {
        return sim.terrain()->get_terrain_height(u.position().x, u.position().z);
    }
};

/// A scene: what it makes before its first beat, what it does before each
/// beat, what must hold after each and at its end, and where it is saved.
struct Scene {
    const char* name;
    int beats;
    int save_at;
    u32 pinned;
    std::function<void(Game&)> setup;
    std::function<void(Game&, int)> before; ///< before beat t
    std::function<void(Game&, int)> after;  ///< after beat t
    std::function<void(Game&)> at_save;     ///< what the save holds
    std::function<void(Game&)> at_end;
};

/// Play `scene`, save it at its save beat, load the save into a fresh game
/// and play both on to the end, beat by beat in step. The checksum both end on.
u32 play(const Scene& scene) {
    Game a;
    CHECK(a.sim.moho_pathing()); // the default
    scene.setup(a);
    std::unique_ptr<Game> b;
    for (int t = 0; t < scene.beats; ++t) {
        if (t == scene.save_at) {
            scene.at_save(a);
            const osc::sim::SavedGame save = osc::sim::save_game(a.sim, scene.name);
            REQUIRE_FALSE(save.snapshot.empty());
            b = std::make_unique<Game>();
            const std::string err = osc::sim::load_snapshot(b->sim, save.snapshot);
            INFO(err);
            REQUIRE(err.empty());
            b->sim.adopt_history(save.game);
            b->ids = a.ids;
            REQUIRE(b->sim.compute_sync_checksum() == a.sim.compute_sync_checksum());
        }
        for (Game* g : {&a, b.get()}) {
            if (!g) continue;
            scene.before(*g, t);
            g->sim.tick();
            scene.after(*g, t);
        }
        if (b) {
            INFO(scene.name << ": beat " << t << " after the save at " << scene.save_at);
            REQUIRE(b->sim.compute_sync_checksum() == a.sim.compute_sync_checksum());
        }
    }
    REQUIRE(b);
    CHECK(b->ids == a.ids);
    scene.at_end(a);
    scene.at_end(*b);
    return a.sim.compute_sync_checksum();
}

void nothing_before(Game&, int) {}

/// Whether each of `units` fits where it stands, beat after beat.
struct FitWatch {
    int misfits = 0;
    void check(const Game& g, size_t from, size_t to) {
        for (size_t n = from; n < to; ++n) {
            const Unit* u = g.unit(n);
            if (u && g.sim.footprint_fits_at(u->footprint(), u->position().x, u->position().z) == 0)
                ++misfits;
        }
    }
};

// --- The crowd: sixteen tanks, a big one in four, through the wall's gap
// six wide, while eight of the other army's cross their way beyond it.

constexpr size_t kCrowd = 16;
constexpr size_t kCrossers = 8;

void make_crowd(Game& g) {
    for (size_t i = 0; i < kCrowd; ++i) {
        const f32 x = 6.5f + static_cast<f32>(i % 4) * 4.0f;
        const f32 z = 44.5f + static_cast<f32>(i / 4) * 8.0f;
        g.make(i % 4 == 3 ? "bigtank" : "tank", 1, x, z);
    }
    for (size_t i = 0; i < kCrossers; ++i)
        g.make("tank", 2, 44.5f, 30.5f + static_cast<f32>(i) * 3.0f);
}

void order_crowd(Game& g, size_t first) {
    for (size_t i = 0; i < kCrowd; ++i) {
        g.move(first + i, 40.5f + static_cast<f32>(i % 4) * 4.0f,
               46.5f + static_cast<f32>(i / 4) * 8.0f);
    }
    for (size_t i = 0; i < kCrossers; ++i)
        g.move(first + kCrowd + i, 44.5f, 92.5f - static_cast<f32>(i) * 3.0f);
}

/// The crowd went through the gap and the crossers across: every move
/// ended, each tank past the wall.
void crowd_arrived(const Game& g, size_t first) {
    for (size_t i = 0; i < kCrowd + kCrossers; ++i) {
        const Unit* u = g.unit(first + i);
        REQUIRE(u);
        INFO("unit " << first + i << " at " << u->position().x << ", " << u->position().z);
        CHECK(u->command_queue().empty());
        CHECK(u->position().x > static_cast<f32>(kWallX1));
    }
}

/// Some of the crowd (those still there) were driving their splines when the game was saved.
void crowd_in_motion(const Game& g, size_t first) {
    int splines = 0;
    for (size_t i = 0; i < kCrowd; ++i) {
        if (const Unit* u = g.unit(first + i)) splines += u->navigator().has_spline() ? 1 : 0;
    }
    CHECK(splines >= 4);
}

Scene crowd_scene(FitWatch& fits) {
    return Scene{
        "crowd",
        900,
        150,
        0xbca546beu,
        [](Game& g) {
            make_crowd(g);
            order_crowd(g, 0);
        },
        nothing_before,
        [&fits](Game& g, int) { fits.check(g, 0, kCrowd + kCrossers); },
        [](Game& g) { crowd_in_motion(g, 0); },
        [](Game& g) { crowd_arrived(g, 0); },
    };
}

// --- ID histories: the same crowd, made from serials an earlier history
// spent. Ten tanks are made and destroyed, one a beat; 99 beats on their
// serials are free again, and the crowd made then takes them, so its ids
// (and with them the collision cells' and the walk's orders, and which
// beat each unit's surface collision runs on) are not the plain crowd's.
// Midway two of the crowd are destroyed, the game is saved while their
// serials are held, and two tanks made once they are free take them and
// join the move.

constexpr size_t kSpent = 10;
constexpr int kCrowdBeat = 110;
constexpr int kKillBeat = 250;
constexpr int kRefillBeat = 380;

struct IdHistory {
    std::vector<u32> spent;  ///< Moho ids of the spent tanks
    std::vector<u32> killed; ///< Moho ids of the two killed midway
};

Scene id_scene(IdHistory& history) {
    return Scene{
        "id histories",
        kRefillBeat + 600,
        kCrowdBeat + 150,
        0xaeaa49aau,
        [](Game&) {},
        [&history](Game& g, int t) {
            if (t < static_cast<int>(kSpent)) {
                const auto n = static_cast<size_t>(t);
                const EntityId id = g.make("tank", 1, 10.5f + static_cast<f32>(t) * 2.0f, 100.5f);
                if (history.spent.size() < kSpent) history.spent.push_back(id.moho());
                g.lua("moho.entity_methods.Destroy(units[" + std::to_string(n + 1) + "])");
            } else if (t == kCrowdBeat) {
                make_crowd(g);
                order_crowd(g, kSpent);
            } else if (t == kKillBeat) {
                for (const size_t n : {kSpent + 1, kSpent + 6}) {
                    if (history.killed.size() < 2) history.killed.push_back(g.ids[n].moho());
                    g.lua("moho.entity_methods.Destroy(units[" + std::to_string(n + 1) + "])");
                }
            } else if (t == kRefillBeat) {
                for (int i = 0; i < 2; ++i) {
                    const size_t n = g.ids.size();
                    g.make("tank", 1, 12.5f, 60.5f + static_cast<f32>(i) * 4.0f);
                    g.move(n, 35.5f, 58.5f + static_cast<f32>(i) * 4.0f);
                }
            }
        },
        [](Game&, int) {},
        [&history](Game& g) {
            // Saved with the two killed gone and their serials held.
            REQUIRE(history.killed.size() == 2);
            CHECK(g.unit(kSpent + 1) == nullptr);
            CHECK(g.unit(kSpent + 6) == nullptr);
            crowd_in_motion(g, kSpent);
        },
        [&history](Game& g) {
            // The crowd took the spent serials; the refills, the killed ones.
            int reused = 0;
            for (size_t i = 0; i < kCrowd + kCrossers; ++i) {
                const u32 moho = g.ids[kSpent + i].moho();
                reused += std::count(history.spent.begin(), history.spent.end(), moho) > 0 ? 1 : 0;
            }
            CHECK(reused == static_cast<int>(kSpent));
            const size_t refill = kSpent + kCrowd + kCrossers;
            for (size_t i = 0; i < 2; ++i) {
                const u32 moho = g.ids[refill + i].moho();
                CHECK(std::count(history.killed.begin(), history.killed.end(), moho) == 1);
                const Unit* u = g.unit(refill + i);
                REQUIRE(u);
                CHECK(u->command_queue().empty());
                CHECK(u->position().x > static_cast<f32>(kWallX1));
            }
            for (size_t i = 0; i < kCrowd + kCrossers; ++i) {
                if (i == 1 || i == 6) continue;
                const Unit* u = g.unit(kSpent + i);
                REQUIRE(u);
                INFO("unit " << kSpent + i);
                CHECK(u->command_queue().empty());
                CHECK(u->position().x > static_cast<f32>(kWallX1));
            }
        },
    };
}

// --- Stun: two rows of five drive north across the open ground, one
// behind the other. Midway two of the front row are stunned for two
// seconds and one for four. Each stands where it was stopped while the
// others go round. Moho's path navigator counts a beat it doesn't move as
// no progress unless it is UNITSTATE_Immobile, which a stun doesn't set
// (CAiPathNavigator::UpdateCurrentPosition, Steam exe 0x5B55BF), and past
// 30 such beats it stops there, as arrived: the two go on to their goals
// once let go, and the one held four seconds gives its move up where it
// stood.

constexpr size_t kRow = 5;
constexpr int kStunBeat = 60;
constexpr size_t kLongStunned = kRow + 2;

/// Beats tank `n` of the front row is stunned for.
int stun_beats(size_t n) {
    return n == kLongStunned ? 40 : 20;
}

struct StunWatch {
    std::vector<osc::sim::Vector3> held; ///< where each stunned tank stood
    int moved_while_stunned = 0;
};

Scene stun_scene(StunWatch& watch) {
    return Scene{
        "stun",
        600,
        kStunBeat + 10,
        0x6bbf7201u,
        [](Game& g) {
            for (size_t c = 0; c < 2; ++c) {
                for (size_t i = 0; i < kRow; ++i) {
                    g.make("tank", 1, 36.5f + static_cast<f32>(i) * 3.0f,
                           20.5f + static_cast<f32>(c) * 3.0f);
                }
            }
            for (size_t n = 0; n < 2 * kRow; ++n)
                g.move(n, 56.5f - static_cast<f32>(n % kRow) * 3.0f,
                       100.5f + static_cast<f32>(n / kRow) * 3.0f);
        },
        [](Game& g, int t) {
            if (t == kStunBeat) {
                for (size_t n = kRow; n < kRow + 3; ++n)
                    g.lua("units[" + std::to_string(n + 1) + "]:SetStunned(" +
                          std::to_string(stun_beats(n) / 10) + ")");
            }
        },
        [&watch](Game& g, int t) {
            if (t == kStunBeat) {
                watch.held.clear();
                for (size_t n = kRow; n < kRow + 3; ++n) {
                    CHECK(g.at(n).is_stunned());
                    watch.held.push_back(g.at(n).position());
                }
                return;
            }
            for (size_t n = kRow; n < kRow + 3; ++n) {
                if (t <= kStunBeat || t >= kStunBeat + stun_beats(n) - 1) continue;
                const osc::sim::Vector3& p = g.at(n).position();
                const osc::sim::Vector3& at = watch.held[n - kRow];
                if (p.x != at.x || p.z != at.z) ++watch.moved_while_stunned;
            }
        },
        [](Game& g) {
            int stunned = 0;
            for (size_t n = kRow; n < kRow + 3; ++n) stunned += g.at(n).is_stunned() ? 1 : 0;
            CHECK(stunned == 3);
        },
        [&watch](Game& g) {
            for (size_t n = 0; n < 2 * kRow; ++n) {
                const Unit* u = &g.at(n);
                INFO("tank " << n << " at " << u->position().x << ", " << u->position().z);
                CHECK_FALSE(u->is_stunned());
                CHECK(u->command_queue().empty());
                if (n == kLongStunned) {
                    // Its move given up: it never drove on from where it was stopped
                    // (the margin for the pushes of tanks going past)
                    REQUIRE(watch.held.size() == 3);
                    CHECK(u->position().z < watch.held[n - kRow].z + 4.0f);
                } else {
                    CHECK(u->position().z > 90.0f);
                }
            }
        },
    };
}

// --- Water: three frigates and a sub sail the lake from its south end to
// its north round the island; two hovercraft cross it from the west shore
// to the east, over the water; two amphibious tanks cross it on its bed.

constexpr size_t kShips = 4;
constexpr size_t kHovers = 2;
constexpr size_t kAmphibs = 2;

struct WaterWatch {
    int ships_aground = 0;       ///< beats a ship stood over ground at or above the water
    int ships_off_water = 0;     ///< beats a ship was on a layer other than Water or Sub
    int amphib_seabed_beats = 0; ///< beats an amphibious tank was on the Seabed
    int hover_water_beats = 0;   ///< beats a hovercraft was on the Water layer
};

Scene water_scene(WaterWatch& watch) {
    return Scene{
        "water",
        900,
        200,
        0x10b6d100u,
        [](Game& g) {
            g.make("frigate", 1, 78.5f, 10.5f);
            g.make("frigate", 1, 84.5f, 14.5f);
            g.make("frigate", 1, 100.5f, 10.5f);
            g.make("sub", 1, 96.0f, 18.0f);
            g.make("hover", 1, 50.5f, 20.5f);
            g.make("hover", 1, 50.5f, 26.5f);
            g.make("amphib", 1, 50.5f, 100.5f);
            g.make("amphib", 1, 50.5f, 106.5f);
            g.move(0, 80.5f, 116.5f);
            g.move(1, 100.5f, 112.5f);
            g.move(2, 84.5f, 120.5f);
            g.move(3, 98.0f, 118.0f);
            g.move(4, 122.5f, 24.5f);
            g.move(5, 122.5f, 30.5f);
            g.move(6, 122.5f, 96.5f);
            g.move(7, 122.5f, 104.5f);
        },
        nothing_before,
        [&watch](Game& g, int) {
            for (size_t n = 0; n < kShips; ++n) {
                const Unit* u = &g.at(n);
                if (g.ground(*u) >= kWaterLevel) ++watch.ships_aground;
                if (u->layer() != "Water" && u->layer() != "Sub") ++watch.ships_off_water;
            }
            for (size_t n = kShips; n < kShips + kHovers; ++n)
                watch.hover_water_beats += g.at(n).layer() == "Water" ? 1 : 0;
            for (size_t n = kShips + kHovers; n < kShips + kHovers + kAmphibs; ++n)
                watch.amphib_seabed_beats += g.at(n).layer() == "Seabed" ? 1 : 0;
        },
        [](Game& g) {
            int splines = 0;
            for (size_t n = 0; n < kShips + kHovers + kAmphibs; ++n)
                splines += g.at(n).navigator().has_spline() ? 1 : 0;
            CHECK(splines >= 4);
        },
        [](Game& g) {
            for (size_t n = 0; n < kShips + kHovers + kAmphibs; ++n) {
                const Unit* u = &g.at(n);
                INFO(u->blueprint_id() << " " << n << " at " << u->position().x << ", "
                                       << u->position().z << " on " << u->layer());
                CHECK(u->command_queue().empty());
            }
            for (size_t n = 0; n < kShips; ++n) CHECK(g.at(n).position().z > 100.0f);
            for (size_t n = kShips; n < kShips + kHovers + kAmphibs; ++n) {
                CHECK(g.at(n).position().x > 112.0f);
                CHECK(g.at(n).layer() == "Land");
            }
        },
    };
}

/// Check the scene's pinned checksum, printing the one it reached.
void check_pinned(const Scene& scene, u32 reached) {
    INFO(scene.name << ": checksum " << std::hex << reached);
    CHECK(reached == scene.pinned);
}

} // namespace

TEST_CASE("Default pathing reference: a crowd through a gap, crossed beyond it",
          "[determinism][crossplatform][moho_pathing]") {
    FitWatch fits;
    const Scene scene = crowd_scene(fits);
    const u32 reached = play(scene);
    // No tank's footprint ever stood where it doesn't fit: in the wall, or
    // over another's ground.
    CHECK(fits.misfits == 0);
    check_pinned(scene, reached);
}

TEST_CASE("Default pathing reference: the crowd made from spent ids, two killed and refilled",
          "[determinism][crossplatform][moho_pathing]") {
    IdHistory history;
    const Scene scene = id_scene(history);
    const u32 reached = play(scene);
    check_pinned(scene, reached);
}

TEST_CASE("Default pathing reference: stunned tanks hold where they stopped, then go on",
          "[determinism][crossplatform][moho_pathing]") {
    StunWatch watch;
    const Scene scene = stun_scene(watch);
    const u32 reached = play(scene);
    CHECK(watch.moved_while_stunned == 0);
    check_pinned(scene, reached);
}

TEST_CASE("Default pathing reference: ships round an island, hovercraft over the lake, "
          "amphibious tanks across its bed",
          "[determinism][crossplatform][moho_pathing]") {
    WaterWatch watch;
    const Scene scene = water_scene(watch);
    const u32 reached = play(scene);
    CHECK(watch.ships_aground == 0);
    CHECK(watch.ships_off_water == 0);
    CHECK(watch.hover_water_beats > 0);
    CHECK(watch.amphib_seabed_beats > 0);
    check_pinned(scene, reached);
}
