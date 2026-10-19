// Hovering aircraft circling (roadmap item 6): Moho's CalcCirclingOrientation.
// A gunship handed its target circles it at its weapon's reach, facing it;
// a drone circles what it works on. HoverOverAttack aircraft, and winged
// ones, never circle.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/air_combat.hpp"
#include "sim/bone_data.hpp"
#include "sim/saved_game.hpp"
#include "sim/sim_random.hpp"
#include "sim/sim_snapshot.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

using osc::f32;
using osc::u32;
using osc::sim::AirCombatRules;
using osc::sim::AirCombatState;
using osc::sim::CirclingInput;
using osc::sim::SimRandom;
using osc::sim::Unit;
using osc::sim::Vector3;

namespace {

constexpr f32 kPi = 3.14159265f;

f32 wrap(f32 a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}

/// `size` square, its height rising one a unit eastward (x) when `slope`,
/// else flat at 10; under `water` if that is above 0.
std::unique_ptr<osc::map::Terrain> terrain(u32 size, bool slope, f32 water = 0.0f) {
    std::vector<osc::u16> heights(static_cast<size_t>(size + 1) * (size + 1), 1280);
    if (slope)
        for (size_t z = 0; z <= size; ++z)
            for (size_t x = 0; x <= size; ++x)
                heights[z * (size + 1) + x] = static_cast<osc::u16>(x * 128);
    osc::map::Heightmap hm(size, size, 1.0f / 128.0f, std::move(heights));
    return std::make_unique<osc::map::Terrain>(std::move(hm), water, water > 0.0f);
}

/// A sim on a flat 128 map with Moho-like gunships ('gunship'; 'steady'
/// never changes its way round), one that hovers over its target
/// ('hoverer'), a winged one ('plane') and a drone.
struct World {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    World() {
        osc::sim::GameSetup game;
        game.scenario = "/maps/test/test_scenario.lua";
        game.seed = 7;
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        sim.set_terrain(terrain(128, false));
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.set_game_setup(game);
        lua_State* L = state.raw();
        const std::string airframe =
            "CanFly = true, MaxAirspeed = 12, MinAirspeed = 3, StartTurnDistance = 5,"
            " KMove = 0.8, KMoveDamping = 2, KTurn = 0.8, KTurnDamping = 1.5";
        const std::string gunship_air = airframe + ", BankFactor = 0.1";
        // return {BlueprintId = '<id>', <body>, Air = {<air>}}
        const auto blueprint = [](const char* id, const std::string& air) {
            std::string s = "return {BlueprintId = '";
            s += id;
            s += "', Categories = {'AIR', 'MOBILE'}, Defense = {MaxHealth = 100},"
                 " SizeX = 2, SizeZ = 2, Footprint = {SizeX = 2, SizeZ = 2},"
                 " Physics = {MotionType = 'RULEUMT_Air', Elevation = 10}, Air = {";
            s += air;
            s += "}}";
            return s;
        };
        for (
            const std::string& bp : {
                blueprint("gunship", gunship_air),
                blueprint("steady", gunship_air + ", CirclingDirChange = false"),
                blueprint("hoverer", gunship_air + ", HoverOverAttack = true"),
                blueprint("plane", gunship_air + ", Winged = true"),
                blueprint("banker", airframe + ", Winged = true, BankFactor = 2"),
                blueprint("flatwing", airframe + ", Winged = true, BankFactor = 0"),
                blueprint("gunfwd", gunship_air + ", BankForward = true"),
                blueprint("nomin", "CanFly = true, MaxAirspeed = 12, StartTurnDistance = 5,"
                                   " KMove = 0.8, KMoveDamping = 2, KTurn = 0.8,"
                                   " KTurnDamping = 1.5, BankFactor = 0.1,"
                                   " CirclingDirChange = false"),
                blueprint("drone", "CanFly = true, MaxAirspeed = 4, MinAirspeed = 3,"
                                   " StartTurnDistance = 5, KMove = 1, KMoveDamping = 1.2"),
                // An engineering drone: it repairs what it guards (REPAIR, a
                // build rate), as the UEF commander's does
                std::string(
                    "return {BlueprintId = 'repairdrone', Categories = {'AIR', 'MOBILE', 'REPAIR'},"
                    " Defense = {MaxHealth = 100}, SizeX = 1, SizeZ = 1,"
                    " Footprint = {SizeX = 1, SizeZ = 1}, Economy = {BuildRate = 5,"
                    " MaxBuildDistance = 4}, Physics = {MotionType = 'RULEUMT_Air', Elevation = 10,"
                    " MaxSpeed = 6},"
                    " Air = {CanFly = true, MaxAirspeed = 6, MinAirspeed = 3, StartTurnDistance = "
                    "5,"
                    " KMove = 1, KMoveDamping = 1.2}}"),
                std::string(
                    "return {BlueprintId = 'tank', Categories = {'LAND', 'MOBILE'},"
                    " Defense = {MaxHealth = 100}, SizeX = 1, SizeZ = 1,"
                    " Economy = {BuildTime = 100, BuildCostMass = 50, BuildCostEnergy = 100},"
                    " Footprint = {SizeX = 1, SizeZ = 1},"
                    " Physics = {MotionType = 'RULEUMT_Land', MaxSpeed = 2}}"),
            }) {
            REQUIRE(state.do_string(bp).ok());
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
        for (const char* id : {"gunship", "steady", "hoverer", "plane", "banker", "flatwing",
                               "gunfwd", "nomin", "drone", "repairdrone", "tank"}) {
            lua_pushstring(L, id);
            lua_getglobal(L, "Plain");
            lua_rawset(L, -3);
        }
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_settop(L, 0);
    }

    /// A second army, ARMY_1's enemy, in a game no one wins.
    void foe() {
        sim.add_army("ARMY_2", "ARMY_2");
        sim.set_victory_condition("sandbox");
    }

    Unit* make(const char* bp, f32 x, f32 z, int army = 1) {
        REQUIRE(state
                    .do_string("made = CreateUnit('" + std::string(bp) + "', " +
                               std::to_string(army) + ", " + std::to_string(x) + ", 20, " +
                               std::to_string(z) + ")")
                    .ok());
        Unit* found = nullptr;
        sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
            auto& u = static_cast<Unit&>(e);
            if (u.blueprint_id() == bp && u.position().x == x && u.position().z == z) found = &u;
        });
        REQUIRE(found);
        lua_settop(state.raw(), 0);
        return found;
    }

    /// A gun of reach `range` that holds its fire (no scripts to fire it).
    static void arm(Unit& u, f32 range) {
        auto gun = std::make_unique<osc::sim::Weapon>();
        gun->max_range = range;
        gun->damage = 10.0f;
        u.add_weapon(std::move(gun));
    }

    static void attack_ground(Unit& u, Vector3 at) {
        osc::sim::UnitCommand cmd;
        cmd.type = osc::sim::CommandType::Attack;
        cmd.target_pos = at;
        u.push_command(cmd, true);
    }

    static void attack(Unit& u, const Unit& target) {
        osc::sim::UnitCommand cmd;
        cmd.type = osc::sim::CommandType::Attack;
        cmd.target_id = target.entity_id();
        cmd.target_pos = target.position();
        u.push_command(cmd, true);
    }

    void tick(Unit& u) {
        for (const auto& w : u.weapons()) w->fire_clock = 1000000;
        sim.tick();
    }

    int run_until(Unit& u, const std::function<bool()>& done, int limit) {
        int t = 0;
        for (; t < limit && !done(); ++t) tick(u);
        return t;
    }
};

bool engaged(const Unit& u) {
    return !u.command_queue().empty() && u.command_queue().front().engaged;
}

f32 flat_dist(const Unit& u, Vector3 at) {
    return std::hypot(u.position().x - at.x, u.position().z - at.z);
}

} // namespace

TEST_CASE("circling_draws: Moho's re-pick, only past its timeout", "[air_circling]") {
    AirCombatRules r;
    AirCombatState st;
    SimRandom rng(11);
    st.timeout_tick = 50;
    circling_draws(st, r, 10.0f, 50, rng);
    CHECK(st.circle_radius_ratio == 1.0f); // not yet
    CHECK(st.circle_elevation == 0.0f);
    bool both_ways[2] = {false, false};
    for (u32 tick = 51; tick < 2000; ++tick) {
        const u32 before = st.timeout_tick;
        circling_draws(st, r, 10.0f, tick, rng);
        if (tick <= before) continue;
        CHECK(st.circle_radius_ratio >= 0.6f);
        CHECK(st.circle_radius_ratio < 0.9f);
        CHECK(std::abs(st.circle_elevation) <= 2.5f); // 0.25 x 10
        // CirclingFlightChangeFrequency 2: 20 to (not) 40 ticks on.
        CHECK(st.timeout_tick >= tick + 20);
        CHECK(st.timeout_tick < tick + 40);
        both_ways[st.circle_reverse ? 1 : 0] = true;
    }
    CHECK(both_ways[0]);
    CHECK(both_ways[1]);

    // The ticks are floored: 1.55 s is 15 to 31 ticks.
    r.circling_change_frequency = 1.55f;
    for (u32 i = 0; i < 200; ++i) {
        st.timeout_tick = 0;
        circling_draws(st, r, 10.0f, 1, rng);
        CHECK(st.timeout_tick >= 1 + 15);
        CHECK(st.timeout_tick < 1 + 31);
    }

    // Without CirclingDirChange the way round stays, and one draw fewer is
    // made: the next draw is the one the other stream made a draw later.
    r.circling_dir_change = false;
    SimRandom a(5);
    SimRandom b(5);
    AirCombatState sa;
    sa.circle_reverse = true;
    circling_draws(sa, r, 10.0f, 1, a);
    CHECK(sa.circle_reverse);
    for (int i = 0; i < 3; ++i) b.next_u32();
    CHECK(a.next_u32() == b.next_u32());
}

TEST_CASE("circling_steer: a step round the circle, at its height over the terrain there",
          "[air_circling]") {
    const auto slope = terrain(128, true);
    CirclingInput in;
    in.position = {50.0f, 30.0f, 40.0f};
    in.center = {50.0f, 0.0f, 60.0f}; // due north (+z)
    in.radius = 15.0f;
    in.min_airspeed = 3.0f;
    in.max_airspeed = 12.0f;
    in.height = 10.0f;

    const auto out = osc::sim::circling_steer(in, slope.get());
    // On the circle, and a step to one side of the aircraft's bearing: by
    // default Moho's +90 degree yaw of the way to the centre, (0, 1) to (1, 0).
    CHECK(std::abs(std::hypot(out.aim.x - 50.0f, out.aim.z - 60.0f) - 15.0f) < 1e-3f);
    CHECK(out.aim.x > 50.0f);
    // Nose at the centre.
    CHECK(std::abs(wrap(out.facing - 0.0f)) < 1e-5f);
    // Height: 10 over the terrain at the aim's nearest whole x.
    CHECK(out.aim.y == 10.0f + std::nearbyint(out.aim.x));
    // At most MaxAirspeed, straight at the aim.
    const f32 speed = std::sqrt(out.velocity.x * out.velocity.x + out.velocity.y * out.velocity.y +
                                out.velocity.z * out.velocity.z);
    CHECK(std::abs(speed - 12.0f) < 1e-3f);

    // The other way round (the -90 degree yaw), the other side.
    in.reverse = true;
    CHECK(osc::sim::circling_steer(in, slope.get()).aim.x < 50.0f);

    // No MinAirspeed: the circle's nearest point, no step round.
    in.min_airspeed = 0.0f;
    const auto still = osc::sim::circling_steer(in, slope.get());
    CHECK(std::abs(still.aim.x - 50.0f) < 1e-4f);
    CHECK(std::abs(still.aim.z - 45.0f) < 1e-4f);

    // Close by, slower than MaxAirspeed: just the way to the aim.
    in.position = {50.0f, still.aim.y, 44.0f};
    const auto near = osc::sim::circling_steer(in, slope.get());
    CHECK(std::abs(near.velocity.z - 1.0f) < 1e-4f);

    // Overhead, it takes due east as the way to the centre.
    in.position = in.center;
    CHECK(std::abs(wrap(osc::sim::circling_steer(in, slope.get()).facing - kPi / 2)) < 1e-5f);
}

TEST_CASE("circling_steer: over water, its height is over the surface, unless it flies in water",
          "[air_circling]") {
    const auto shore = terrain(128, true, 50.0f);
    CirclingInput in;
    in.position = {30.0f, 70.0f, 40.0f};
    in.center = {30.0f, 0.0f, 60.0f};
    in.radius = 15.0f;
    in.min_airspeed = 3.0f;
    in.max_airspeed = 12.0f;
    in.height = 10.0f;

    CHECK(osc::sim::circling_steer(in, shore.get()).aim.y == 60.0f);
    in.fly_in_water = true;
    const auto under = osc::sim::circling_steer(in, shore.get());
    CHECK(under.aim.y == 10.0f + std::nearbyint(under.aim.x));

    in.position.x = 90.0f;
    in.center.x = 90.0f;
    for (const bool fly_in_water : {false, true}) {
        in.fly_in_water = fly_in_water;
        const auto land = osc::sim::circling_steer(in, shore.get());
        CHECK(land.aim.y == 10.0f + std::nearbyint(land.aim.x));
    }
}

TEST_CASE("Only a hovering flier circles: not winged, not HoverOverAttack, not under "
          "construction",
          "[air_circling]") {
    World w;
    CHECK(osc::sim::circles(*w.make("gunship", 20.0f, 20.0f)));
    CHECK_FALSE(osc::sim::circles(*w.make("plane", 30.0f, 20.0f)));
    CHECK_FALSE(osc::sim::circles(*w.make("hoverer", 40.0f, 20.0f)));
    Unit& built = *w.make("gunship", 50.0f, 20.0f);
    built.set_is_being_built(true);
    CHECK_FALSE(osc::sim::circles(built));
    // The blueprint's fields, and Moho's defaults for those it leaves out.
    const AirCombatRules& r = w.make("steady", 60.0f, 20.0f)->air_combat_rules();
    CHECK_FALSE(r.circling_dir_change);
    CHECK(r.circling_min_airspeed == 3.0f);
    CHECK(r.circling_turn_mult == 3.0f);
    CHECK(r.circling_radius_min == 0.6f);
    CHECK(r.circling_radius_max == 0.9f);
    CHECK(r.bank_factor == 0.1f);
    CHECK(w.make("gunship", 70.0f, 20.0f)->air_combat_rules().circling_dir_change);
}

TEST_CASE("A gunship handed a ground target circles it within its weapon's reach, facing it, "
          "at its attack height",
          "[air_circling]") {
    World w;
    Unit& g = *w.make("gunship", 20.0f, 64.0f);
    World::arm(g, 22.0f);
    const Vector3 at{64.0f, 10.0f, 64.0f};
    World::attack_ground(g, at);
    REQUIRE(w.run_until(g, [&] { return engaged(g); }, 300) < 300);

    for (int t = 0; t < 100; ++t) w.tick(g); // settle onto the circle
    f32 nearest = 1e9f;
    f32 farthest = 0.0f;
    f32 worst_facing = 0.0f;
    f32 path = 0.0f;
    f32 low = 1e9f;
    f32 high = -1e9f;
    Vector3 last = g.position();
    for (int t = 0; t < 300; ++t) {
        w.tick(g);
        const Vector3 p = g.position();
        nearest = std::min(nearest, flat_dist(g, at));
        farthest = std::max(farthest, flat_dist(g, at));
        const f32 bearing = std::atan2(at.x - p.x, at.z - p.z);
        worst_facing = std::max(worst_facing, std::abs(wrap(bearing - g.heading())));
        path += std::hypot(p.x - last.x, p.z - last.z);
        low = std::min(low, p.y);
        high = std::max(high, p.y);
        last = p;
    }
    INFO("nearest " << nearest << " farthest " << farthest << " facing " << worst_facing << " path "
                    << path << " y " << low << ".." << high);
    CHECK(engaged(g));
    CHECK(nearest > 0.6f * 22.0f - 2.0f);
    CHECK(farthest < 0.9f * 22.0f + 2.0f);
    CHECK(worst_facing < 0.35f);
    CHECK(path > 15.0f); // it moves, unlike a parked attacker
    // AttackElevation (its Elevation, 10) +- a quarter, over the ground at 10.
    CHECK(low > 20.0f - 2.5f - 0.5f);
    CHECK(high < 20.0f + 2.5f + 0.5f);
    CHECK(g.air_combat().flying);
}

TEST_CASE("Without CirclingDirChange a gunship goes round one way only", "[air_circling]") {
    World w;
    Unit& g = *w.make("steady", 20.0f, 64.0f);
    World::arm(g, 22.0f);
    const Vector3 at{64.0f, 10.0f, 64.0f};
    World::attack_ground(g, at);
    REQUIRE(w.run_until(g, [&] { return engaged(g); }, 300) < 300);
    for (int t = 0; t < 100; ++t) w.tick(g);
    f32 net = 0.0f;
    f32 total = 0.0f;
    f32 angle = std::atan2(g.position().x - at.x, g.position().z - at.z);
    for (int t = 0; t < 300; ++t) {
        w.tick(g);
        const f32 now = std::atan2(g.position().x - at.x, g.position().z - at.z);
        const f32 step = wrap(now - angle);
        net += step;
        total += std::abs(step);
        angle = now;
    }
    INFO("net " << net << " total " << total);
    CHECK(std::abs(net) > 2.0f);
    CHECK(std::abs(net) > 0.95f * total);
}

TEST_CASE("A gunship without MinAirspeed goes round its target by a MaxAirspeed's step",
          "[air_circling]") {
    World w;
    Unit& g = *w.make("nomin", 20.0f, 64.0f);
    World::arm(g, 22.0f);
    const Vector3 at{64.0f, 10.0f, 64.0f};
    World::attack_ground(g, at);
    REQUIRE(w.run_until(g, [&] { return engaged(g); }, 300) < 300);
    for (int t = 0; t < 100; ++t) {
        w.tick(g);
    }
    f32 net = 0.0f;
    f32 angle = std::atan2(g.position().x - at.x, g.position().z - at.z);
    for (int t = 0; t < 300; ++t) {
        w.tick(g);
        const f32 now = std::atan2(g.position().x - at.x, g.position().z - at.z);
        net += wrap(now - angle);
        angle = now;
    }
    INFO("net " << net);
    CHECK(std::abs(net) > 2.0f);
}

TEST_CASE("A HoverOverAttack aircraft flies on past its reach and hangs over its target rather "
          "than circle",
          "[air_circling]") {
    World w;
    Unit& h = *w.make("hoverer", 20.0f, 64.0f);
    World::arm(h, 22.0f);
    Vector3 at{64.5f, 10.0f, 64.5f};
    SECTION("a point on the ground") {
        World::attack_ground(h, at);
    }
    SECTION("a unit") {
        const Unit& target = *w.make("tank", 64.0f, 64.0f);
        at = target.position();
        World::attack(h, target);
    }
    REQUIRE(w.run_until(h, [&] { return flat_dist(h, at) < 22.0f; }, 300) < 300);
    f32 nearest = 1e9f;
    Vector3 last = h.position();
    for (int t = 0; t < 300; ++t) {
        last = h.position();
        w.tick(h);
        nearest = std::min(nearest, flat_dist(h, at));
    }
    INFO("nearest " << nearest << " final " << flat_dist(h, at));
    CHECK(nearest < 1.0f);
    CHECK(flat_dist(h, at) < 1.0f);
    CHECK(flat_dist(h, last) < 0.02f);
    CHECK_FALSE(h.air_combat().flying);
}

TEST_CASE("A unit that can't fly stops at its weapon's reach of its attack target",
          "[air_circling]") {
    World w;
    Unit& g = *w.make("tank", 20.0f, 64.0f);
    World::arm(g, 22.0f);
    Vector3 at{64.5f, 10.0f, 64.5f};
    SECTION("a point on the ground") {
        World::attack_ground(g, at);
    }
    SECTION("a unit") {
        const Unit& target = *w.make("tank", 64.0f, 64.0f);
        at = target.position();
        World::attack(g, target);
    }
    REQUIRE(w.run_until(g, [&] { return flat_dist(g, at) < 22.0f; }, 600) < 600);
    f32 nearest = 1e9f;
    for (int t = 0; t < 100; ++t) {
        w.tick(g);
        nearest = std::min(nearest, flat_dist(g, at));
    }
    INFO("nearest " << nearest);
    CHECK(nearest > 20.0f);
    CHECK_FALSE(g.navigator().busy());
}

namespace {

/// A skeleton whose bones past its root, at `at`, are its target bones.
osc::sim::BoneData target_bones(const std::vector<Vector3>& at) {
    osc::sim::BoneData bd;
    bd.bones.emplace_back();
    for (const Vector3& p : at) {
        osc::sim::BoneInfo b;
        b.parent_index = 0;
        b.local_position = p;
        b.world_position = p;
        bd.target_bones.push_back(static_cast<osc::i32>(bd.bones.size()));
        bd.bones.push_back(b);
    }
    return bd;
}

osc::sim::UnitCommand attack_order(const Unit& target) {
    osc::sim::UnitCommand cmd;
    cmd.type = osc::sim::CommandType::Attack;
    cmd.target_id = target.entity_id();
    cmd.target_pos = target.position();
    return cmd;
}

} // namespace

TEST_CASE("A gunship circles its order's target point on a unit, not the unit's centre",
          "[air_circling]") {
    World w;
    w.foe();
    Unit& g = *w.make("steady", 20.0f, 64.0f);
    World::arm(g, 22.0f);
    Unit& target = *w.make("tank", 64.0f, 64.0f, 2);
    const osc::sim::BoneData bones = target_bones({{8.0f, 0.0f, 0.0f}});
    target.set_bone_data(&bones);
    const Vector3 bow = target.target_point(0);
    REQUIRE(bow.x == 72.0f);
    REQUIRE(w.sim.route_command({g.entity_id()}, attack_order(target), true) != 0);
    REQUIRE(w.run_until(g, [&] { return engaged(g); }, 300) < 300);

    for (int t = 0; t < 100; ++t) {
        w.tick(g);
    }
    f32 nearest = 1e9f;
    f32 farthest = 0.0f;
    for (int t = 0; t < 300; ++t) {
        w.tick(g);
        nearest = std::min(nearest, flat_dist(g, bow));
        farthest = std::max(farthest, flat_dist(g, bow));
    }
    INFO("nearest " << nearest << " farthest " << farthest);
    CHECK(nearest > 0.6f * 22.0f - 2.0f);
    CHECK(farthest < 0.9f * 22.0f + 2.0f);
}

TEST_CASE("An attack order draws its target point once, for all its units", "[air_circling]") {
    World w;
    w.foe();
    const Unit& a = *w.make("gunship", 20.0f, 60.0f);
    const Unit& b = *w.make("gunship", 20.0f, 68.0f);
    Unit& target = *w.make("tank", 64.0f, 64.0f, 2);
    const osc::sim::BoneData bones = target_bones(
        {{0.0f, 0.0f, 3.0f}, {0.0f, 0.0f, -3.0f}, {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}});
    target.set_bone_data(&bones);
    std::set<osc::i32> drawn;
    for (int i = 0; i < 8; ++i) {
        REQUIRE(w.sim.route_command({a.entity_id(), b.entity_id()}, attack_order(target), true) !=
                0);
        const osc::i32 point = a.command_queue().front().target_point;
        CHECK(b.command_queue().front().target_point == point);
        drawn.insert(point);
    }
    CHECK(*drawn.begin() >= 0);
    CHECK(drawn.size() > 1);
}

TEST_CASE("A drone at work circles what it repairs at StartTurnDistance's ratio; it stops "
          "when the work does",
          "[air_circling]") {
    World w;
    Unit& patient = *w.make("tank", 64.0f, 64.0f);
    Unit& d = *w.make("repairdrone", 66.0f, 64.0f);
    // Its repair order: Moho repairs only under one (CUnitRepairTask).
    osc::sim::UnitCommand repair;
    repair.type = osc::sim::CommandType::Repair;
    repair.target_id = patient.entity_id();
    repair.target_pos = patient.position();
    d.push_command(repair, true);
    const auto hurt = [&] { patient.set_health(1.0f); }; // work that never ends
    for (int t = 0; t < 60; ++t) {
        hurt();
        w.tick(d);
    }
    REQUIRE(d.is_repairing());
    f32 nearest = 1e9f;
    f32 farthest = 0.0f;
    for (int t = 0; t < 200; ++t) {
        hurt();
        w.tick(d);
        nearest = std::min(nearest, flat_dist(d, patient.position()));
        farthest = std::max(farthest, flat_dist(d, patient.position()));
    }
    INFO("nearest " << nearest << " farthest " << farthest);
    CHECK(nearest > 0.6f * 5.0f - 1.0f);
    CHECK(farthest < 0.9f * 5.0f + 1.0f);
    CHECK(d.air_combat().flying);

    d.clear_commands(); // stopped: the repair ends with its order
    w.tick(d);
    w.tick(d);
    CHECK_FALSE(d.is_repairing());
    CHECK_FALSE(d.air_combat().flying);
    CHECK(d.air_combat().circle_radius_ratio == 1.0f); // drawn again next time
}

TEST_CASE("A game saved while a gunship circles loads and goes on as the original",
          "[air_circling]") {
    World a;
    Unit& g = *a.make("gunship", 20.0f, 64.0f);
    World::arm(g, 22.0f);
    World::attack_ground(g, {64.0f, 10.0f, 64.0f});
    a.sim.set_recording(true);
    REQUIRE(a.run_until(g, [&] { return engaged(g); }, 300) < 300);
    for (int t = 0; t < 45; ++t) a.tick(g);
    REQUIRE(g.air_combat().circle_radius_ratio != 1.0f);
    const osc::sim::SavedGame save = osc::sim::save_game(a.sim, "circling");
    REQUIRE_FALSE(save.snapshot.empty());
    World b;
    b.make("gunship", 20.0f, 64.0f);
    const std::string err = osc::sim::load_snapshot(b.sim, save.snapshot);
    INFO(err);
    REQUIRE(err.empty());
    Unit* loaded = nullptr;
    b.sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
        if (e.entity_id() == g.entity_id()) loaded = static_cast<Unit*>(&e);
    });
    REQUIRE(loaded);
    for (int t = 0; t < 120; ++t) {
        a.tick(g);
        b.tick(*loaded);
        INFO("tick " << t);
        REQUIRE(b.sim.compute_sync_checksum() == a.sim.compute_sync_checksum());
    }
    CHECK(loaded->position().x == g.position().x);
    CHECK(loaded->position().z == g.position().z);
    CHECK(loaded->air_combat().circle_radius_ratio == g.air_combat().circle_radius_ratio);
}

TEST_CASE("A circling gunship given a move stops circling and flies it", "[air_circling]") {
    World w;
    Unit& g = *w.make("gunship", 20.0f, 64.0f);
    World::arm(g, 22.0f);
    World::attack_ground(g, {64.0f, 10.0f, 64.0f});
    REQUIRE(w.run_until(g, [&] { return engaged(g); }, 300) < 300);
    for (int t = 0; t < 40; ++t) w.tick(g);
    REQUIRE(g.air_combat().flying);
    osc::sim::UnitCommand move;
    move.type = osc::sim::CommandType::Move;
    move.target_pos = {110.0f, 20.0f, 110.0f};
    g.push_command(move, true); // replaces the attack
    w.tick(g);
    w.tick(g);
    CHECK_FALSE(g.air_combat().flying);
    CHECK(g.air_combat().timeout_tick == 0);
    w.run_until(g, [&] { return g.command_queue().empty(); }, 400);
    CHECK(flat_dist(g, {110.0f, 0.0f, 110.0f}) < 6.0f);
}

namespace {

void guard(Unit& u, const Unit& what) {
    osc::sim::UnitCommand cmd;
    cmd.type = osc::sim::CommandType::Guard;
    cmd.target_id = what.entity_id();
    cmd.target_pos = what.position();
    u.push_command(cmd, true);
}

} // namespace

TEST_CASE("A gunship guarding with nothing to do flies as a winged one: nose first onto what it "
          "guards, slowing to rest there, never circling",
          "[air_circling]") {
    World w;
    Unit& charge = *w.make("plane", 64.0f, 64.0f);
    charge.set_is_being_built(true); // stays put: it neither flies nor lands
    Unit& g = *w.make("gunship", 30.0f, 64.0f);
    guard(g, charge);
    w.tick(g);
    CHECK(osc::sim::flies_winged_on_guard(g));
    CHECK_FALSE(osc::sim::circles(g));
    // On its way: its nose leads, the thrust along it (no sideways drift as
    // a hovering flight would)
    f32 worst_slip = 0.0f;
    for (int t = 0; t < 40; ++t) {
        w.tick(g);
        const Vector3& v = g.air_velocity();
        const f32 speed = std::hypot(v.x, v.z);
        if (speed > 1.0f)
            worst_slip = std::max(worst_slip, std::abs(wrap(std::atan2(v.x, v.z) - g.heading())));
    }
    CHECK(worst_slip < 0.6f);
    for (int t = 0; t < 300; ++t) w.tick(g);
    // At rest by what it guards, its approach point just clear of it (not 10
    // off, as a parked guard stops), never having drawn a circle
    const f32 there = flat_dist(g, charge.position());
    INFO("there " << there);
    CHECK(there < 6.0f);
    CHECK(std::hypot(g.air_velocity().x, g.air_velocity().z) < 0.5f);
    CHECK(g.air_combat().flying);
    CHECK(g.air_combat().state == 0);                  // no attack runs
    CHECK(g.air_combat().circle_radius_ratio == 1.0f); // never drew a circle
}

TEST_CASE("Only a hovering aircraft guarding with nothing to do flies winged: not a winged one, "
          "not one attacking, building or repairing; reclaiming doesn't spare it",
          "[air_circling]") {
    World w;
    Unit& charge = *w.make("plane", 64.0f, 64.0f);
    charge.set_is_being_built(true);
    Unit& plane = *w.make("plane", 30.0f, 40.0f);
    guard(plane, charge);
    CHECK_FALSE(osc::sim::flies_winged_on_guard(plane)); // Air.Winged: its own flight

    Unit& d = *w.make("drone", 30.0f, 80.0f);
    guard(d, charge);
    CHECK(osc::sim::flies_winged_on_guard(d));
    d.set_repair_target_id(charge.entity_id());
    CHECK_FALSE(osc::sim::flies_winged_on_guard(d)); // repairing: it hovers, circling
    CHECK(osc::sim::circles(d));
    d.set_repair_target_id(0);
    d.set_reclaim_target_id(charge.entity_id());
    CHECK(osc::sim::flies_winged_on_guard(d)); // reclaiming doesn't count
    d.set_reclaim_target_id(0);

    // The guard's attack is an order of its own at the head: it circles
    Unit& g = *w.make("gunship", 30.0f, 100.0f);
    guard(g, charge);
    osc::sim::UnitCommand attack;
    attack.type = osc::sim::CommandType::Attack;
    attack.target_pos = {80.0f, 10.0f, 100.0f};
    attack.from_guard = true;
    g.push_command(attack, false);
    std::swap(const_cast<std::deque<osc::sim::UnitCommand>&>(g.command_queue()).front(),
              const_cast<std::deque<osc::sim::UnitCommand>&>(g.command_queue()).back());
    CHECK_FALSE(osc::sim::flies_winged_on_guard(g));
    CHECK(osc::sim::circles(g));
}

TEST_CASE("A game saved while a gunship flies its guard winged loads and goes on as the original",
          "[air_circling]") {
    World a;
    Unit& charge = *a.make("plane", 64.0f, 64.0f);
    charge.set_is_being_built(true);
    Unit& g = *a.make("gunship", 30.0f, 64.0f);
    guard(g, charge);
    a.sim.set_recording(true);
    for (int t = 0; t < 120; ++t) a.tick(g);
    REQUIRE(g.air_combat().flying);
    const osc::sim::SavedGame save = osc::sim::save_game(a.sim, "guard");
    REQUIRE_FALSE(save.snapshot.empty());
    World b;
    b.make("plane", 64.0f, 64.0f);
    b.make("gunship", 30.0f, 64.0f);
    const std::string err = osc::sim::load_snapshot(b.sim, save.snapshot);
    INFO(err);
    REQUIRE(err.empty());
    Unit* loaded = nullptr;
    b.sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
        if (e.entity_id() == g.entity_id()) loaded = static_cast<Unit*>(&e);
    });
    REQUIRE(loaded);
    for (int t = 0; t < 120; ++t) {
        a.tick(g);
        b.tick(*loaded);
        INFO("tick " << t);
        REQUIRE(b.sim.compute_sync_checksum() == a.sim.compute_sync_checksum());
    }
    CHECK(loaded->position().x == g.position().x);
    CHECK(loaded->position().z == g.position().z);
}

TEST_CASE("A drone guarding a hurt unit out of its reach flies winged to it, once a tick, then "
          "repairs it hovering",
          "[air_circling]") {
    World w;
    Unit& hurt = *w.make("tank", 90.0f, 64.0f);
    hurt.set_health(40.0f);
    Unit& d = *w.make("repairdrone", 30.0f, 64.0f);
    guard(d, hurt);
    w.tick(d);
    CHECK(osc::sim::flies_winged_on_guard(d));
    // On its way, one flight a tick: each tick it moves by its airframe's own
    // velocity, nothing more (a navigator's move as well would add to it)
    f32 fastest = 0.0f;
    f32 worst_extra = 0.0f;
    Vector3 last = d.position();
    Vector3 last_v = d.air_velocity();
    int t = 0;
    for (; t < 600 && !d.is_repairing(); ++t) {
        w.tick(d);
        const Vector3 p = d.position();
        const f32 moved = std::hypot(p.x - last.x, p.z - last.z);
        fastest = std::max(fastest, moved * 10.0f);
        const Vector3& v = d.air_velocity();
        if (d.air_combat().flying && !d.is_repairing()) {
            const f32 step = std::hypot(v.x + last_v.x, v.z + last_v.z) * 0.05f;
            worst_extra = std::max(worst_extra, std::abs(moved - step));
        }
        last = p;
        last_v = v;
    }
    INFO("ticks " << t << " fastest " << fastest << " worst extra " << worst_extra);
    CHECK(d.is_repairing());
    CHECK(d.repair_target_id() == hurt.entity_id());
    CHECK(fastest <= 6.0f * 1.05f);
    CHECK(worst_extra < 0.01f);
    // At work it hovers, circling what it repairs (Moho: Repairing)
    CHECK_FALSE(osc::sim::flies_winged_on_guard(d));
    CHECK(osc::sim::circles(d));
}

TEST_CASE("A winged aircraft's attack run banks into its turn by its BankFactor, as its moves do",
          "[air_circling]") {
    World w;
    Unit& banked = *w.make("banker", 20.0f, 20.0f);
    Unit& level = *w.make("flatwing", 20.0f, 90.0f);
    World::arm(banked, 200.0f);
    World::arm(level, 200.0f);
    World::attack_ground(banked, {80.0f, 10.0f, 20.0f});
    World::attack_ground(level, {80.0f, 10.0f, 90.0f});
    f32 deepest = 0.0f;
    f32 flattest = 0.0f;
    for (int t = 0; t < 30; ++t) {
        w.tick(banked);
        deepest = std::max(deepest, std::abs(banked.bank_angle()));
        flattest = std::max(flattest, std::abs(level.bank_angle()));
    }
    INFO("deepest " << deepest << " flattest " << flattest);
    REQUIRE(engaged(banked));
    REQUIRE(engaged(level));
    CHECK(deepest > 0.2f);
    CHECK(flattest < 0.01f);
}

TEST_CASE("A circling gunship leans forward as its speed changes only with BankForward",
          "[air_circling]") {
    World w;
    Unit& plain = *w.make("gunship", 20.0f, 30.0f);
    Unit& forward = *w.make("gunfwd", 20.0f, 100.0f);
    World::arm(plain, 22.0f);
    World::arm(forward, 22.0f);
    World::attack_ground(plain, {64.0f, 10.0f, 30.0f});
    World::attack_ground(forward, {64.0f, 10.0f, 100.0f});
    f32 plain_pitch = 0.0f;
    f32 forward_pitch = 0.0f;
    for (int t = 0; t < 200; ++t) {
        w.tick(plain);
        if (engaged(plain)) {
            plain_pitch = std::max(plain_pitch, std::abs(plain.pitch_angle()));
        }
        if (engaged(forward)) {
            forward_pitch = std::max(forward_pitch, std::abs(forward.pitch_angle()));
        }
    }
    INFO("plain " << plain_pitch << " forward " << forward_pitch);
    REQUIRE(engaged(forward));
    CHECK(forward_pitch > 0.1f);
    CHECK(plain_pitch < forward_pitch * 0.5f);
}
