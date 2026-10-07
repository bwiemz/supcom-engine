// Where weapons aim on a unit, as Moho's target points (faf-re
// Unit::GetTargetPoint / PickTargetPoint, UnitWeapon::PickNewTargetAimSpot):
// its blueprint's AI.TargetBones, one picked at random each time a weapon
// takes the unit as its target (above the water for an AboveWaterTargetsOnly
// weapon), the unit's centre with none. A shot homes on its weapon's point,
// and a unit on the seabed is in reach of guns above the water only where a
// target point of it is above the surface (CanAttackTarget).

#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/bone_data.hpp"
#include "sim/manipulator.hpp"
#include "sim/projectile.hpp"
#include "sim/sim_random.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <set>
#include <vector>

using osc::f32;
using osc::i32;
using osc::u32;
using osc::sim::BoneData;
using osc::sim::BoneInfo;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::Vector3;
using osc::sim::Weapon;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

/// A walker's skeleton: a root at its feet and bones `low` and `high` up,
/// the two its target bones.
BoneData walker_bones(f32 low, f32 high) {
    BoneData bd;
    const auto bone = [&](const char* name, f32 y) {
        BoneInfo b;
        b.name = name;
        b.parent_index = bd.bones.empty() ? -1 : 0;
        b.local_position = {0, y, 0};
        b.world_position = {0, y, 0};
        bd.name_to_index[name] = static_cast<i32>(bd.bones.size());
        bd.bones.push_back(b);
    };
    bone("root", 0);
    bone("hip", low);
    bone("head", high);
    bd.target_bones = {1, 2};
    return bd;
}

/// 128 x 128: a seabed at height 0 under water at 5.
std::unique_ptr<osc::map::Terrain> sea() {
    std::vector<osc::u16> heights(129 * 129, 0);
    osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
    return std::make_unique<osc::map::Terrain>(std::move(hm), 5.0f, true);
}

void two_armies(SimState& sim) {
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.set_fog_of_war("none");
    sim.set_victory_condition("sandbox");
}

u32 add_unit(SimState& sim, int army, const Vector3& at, const BoneData* bones) {
    auto u = std::make_unique<Unit>();
    u->set_army(army);
    u->set_position(at);
    u->set_size_y(2.0f);
    if (bones) u->set_bone_data(bones);
    return sim.entity_registry().register_entity(std::move(u));
}

Unit& unit(SimState& sim, u32 id) {
    return static_cast<Unit&>(*sim.entity_registry().find(id));
}

bool same(const Vector3& a, const Vector3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

} // namespace

TEST_CASE("A unit's target points are its TargetBones, else its centre", "[targetpoints]") {
    const BoneData bones = walker_bones(3.0f, 8.0f);
    Unit u;
    u.set_position({10, 0, 20});
    u.set_size_y(2.0f);
    u.set_bone_data(&bones);
    CHECK(u.target_point_count() == 2);
    CHECK(same(u.target_point(0), {10, 3, 20}));
    CHECK(same(u.target_point(1), {10, 8, 20}));
    CHECK(same(u.target_point(7), {10, 8, 20}));  // past the last: the last
    CHECK(same(u.target_point(-1), {10, 1, 20})); // the centre, half its height up

    // A bone the mesh hasn't is the centre too.
    BoneData missing = bones;
    missing.target_bones = {-1};
    u.set_bone_data(&missing);
    CHECK(same(u.target_point(0), {10, 1, 20}));

    Unit plain;
    plain.set_position({10, 0, 20});
    plain.set_size_y(2.0f);
    CHECK(plain.target_point_count() == 0);
    CHECK(same(plain.target_point(0), {10, 1, 20}));
}

TEST_CASE("A target point is picked at random, drawing nothing on a unit with none",
          "[targetpoints]") {
    const BoneData bones = walker_bones(3.0f, 8.0f);
    Unit u;
    u.set_bone_data(&bones);
    osc::sim::SimRandom rng(42);
    std::set<i32> seen;
    for (int i = 0; i < 64; ++i) seen.insert(u.pick_target_point(rng));
    CHECK(seen == std::set<i32>{0, 1});

    Unit plain;
    osc::sim::SimRandom a(7);
    osc::sim::SimRandom b(7);
    CHECK(plain.pick_target_point(a) == -1);
    CHECK(a.next_u64() == b.next_u64());

    // Above or below the water: only the points on that side, the unit's
    // feet with none.
    u.set_position({0, 0, 0});
    for (int i = 0; i < 16; ++i) {
        i32 at = -2;
        CHECK(u.pick_target_point_by_water(&rng, 5.0f, true, at));
        CHECK(at == 1); // the head, at 8
        CHECK(u.pick_target_point_by_water(&rng, 5.0f, false, at));
        CHECK(at == 0); // the hip, at 3
    }
    i32 at = -2;
    CHECK_FALSE(u.pick_target_point_by_water(&rng, 9.0f, true, at));
    CHECK(at == -1);
    CHECK_FALSE(plain.pick_target_point_by_water(nullptr, 5.0f, true, at));
    CHECK(plain.pick_target_point_by_water(nullptr, 5.0f, false, at));
}

TEST_CASE("A weapon aims at the point it picked on its target, and its shot homes on it",
          "[targetpoints]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    two_armies(sim);
    const BoneData bones = walker_bones(3.0f, 8.0f);
    const u32 shooter = add_unit(sim, 0, {0, 0, 0}, nullptr);
    const u32 target = add_unit(sim, 1, {0, 0, 30}, &bones);
    Weapon w;
    w.max_range = 40.0f;
    w.damage = 10.0f;
    w.muzzle_velocity = 20.0f;
    w.lead_target = false;

    // Each time the target changes it picks again; until then, the centre.
    std::set<i32> seen;
    for (int i = 0; i < 32; ++i) {
        w.set_target_entity(0);
        w.pick_aim_spot(sim.entity_registry(), &sim);
        w.set_target_entity(target);
        CHECK(w.current_aim_spot() == -1);
        w.pick_aim_spot(sim.entity_registry(), &sim);
        seen.insert(w.current_aim_spot());
    }
    CHECK(seen == std::set<i32>{0, 1});

    Unit& owner = unit(sim, shooter);
    const Unit& at = unit(sim, target);
    const Vector3 point = at.target_point(w.current_aim_spot());
    CHECK(same(w.aim_point(at, owner, {0, 1, 0}, {0, 0, 1}), point));

    // The shot takes its weapon's point, and steers for it.
    osc::sim::Projectile* shot =
        w.launch(owner, {0, 1, 0}, &at, sim.entity_registry(), nullptr, false, std::nullopt);
    REQUIRE(shot);
    CHECK(shot->target_point == w.current_aim_spot());
    shot->tracking = true;
    shot->target_position = {};
    shot->update(0.1, sim.entity_registry(), nullptr);
    CHECK(same(shot->target_position, point));

    // A unit with no target bones: its centre, and no draw.
    const u32 plain = add_unit(sim, 1, {5, 0, 30}, nullptr);
    w.set_target_entity(plain);
    w.pick_aim_spot(sim.entity_registry(), &sim);
    CHECK(w.current_aim_spot() == -1);
    CHECK(same(w.aim_point(unit(sim, plain), owner, {0, 1, 0}, {0, 0, 1}), {5, 1, 30}));
}

TEST_CASE("A unit on the seabed is in reach above the water only where a target point is",
          "[targetpoints]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_terrain(sea());
    two_armies(sim);
    const BoneData wader = walker_bones(3.0f, 8.0f); // its head out of the water at 5
    const BoneData under = walker_bones(1.0f, 2.0f); // all of it under
    const BoneData tall = walker_bones(6.0f, 8.0f);  // nothing under but its feet
    const u32 shooter = add_unit(sim, 0, {10, 5, 10}, nullptr);
    const auto on_bed = [&](const BoneData* bones) {
        const u32 id = add_unit(sim, 1, {20, 0, 20}, bones);
        unit(sim, id).set_layer("Seabed");
        return id;
    };
    const u32 wading = on_bed(&wader);
    const u32 sunk = on_bed(&under);
    const u32 towering = on_bed(&tall);
    const u32 boneless = on_bed(nullptr);
    const Unit& owner = unit(sim, shooter);

    Weapon gun; // AboveWaterTargetsOnly: a tank's cannon
    gun.above_water_targets_only = true;
    CHECK(gun.can_pick(owner, unit(sim, wading), &sim));
    CHECK(gun.can_pick(owner, unit(sim, towering), &sim));
    CHECK_FALSE(gun.can_pick(owner, unit(sim, sunk), &sim));
    CHECK_FALSE(gun.can_pick(owner, unit(sim, boneless), &sim)); // its feet are under

    Weapon torpedo; // BelowWaterTargetsOnly
    torpedo.below_water_targets_only = true;
    CHECK(torpedo.can_pick(owner, unit(sim, wading), &sim));
    CHECK(torpedo.can_pick(owner, unit(sim, sunk), &sim));
    CHECK_FALSE(torpedo.can_pick(owner, unit(sim, towering), &sim));
    CHECK(torpedo.can_pick(owner, unit(sim, boneless), &sim));

    // The gun aims at the head, the torpedo at the hip, every time.
    for (int i = 0; i < 8; ++i) {
        gun.set_target_entity(0);
        gun.pick_aim_spot(sim.entity_registry(), &sim);
        gun.set_target_entity(wading);
        gun.pick_aim_spot(sim.entity_registry(), &sim);
        CHECK(gun.current_aim_spot() == 1);
        torpedo.set_target_entity(0);
        torpedo.pick_aim_spot(sim.entity_registry(), &sim);
        torpedo.set_target_entity(wading);
        torpedo.pick_aim_spot(sim.entity_registry(), &sim);
        CHECK(torpedo.current_aim_spot() == 0);
    }
}

TEST_CASE("TransferTarget gives the second weapon the first's target; SetTargetEntity picks at "
          "once",
          "[targetpoints][lua]") {
    osc::lua::LuaState lua;
    SimState sim(lua.raw(), nullptr);
    osc::lua::register_sim_bindings(lua, sim);
    osc::lua::register_moho_bindings(lua, sim);
    two_armies(sim);
    const BoneData bones = walker_bones(3.0f, 8.0f);
    const u32 target = add_unit(sim, 1, {0, 0, 30}, &bones);
    Weapon from;
    Weapon to;
    lua_State* L = lua.raw();
    const auto handle = [&](const char* name, void* object) {
        lua_newtable(L);
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, object);
        lua_rawset(L, -3);
        lua_setglobal(L, name);
    };
    handle("from", &from);
    handle("to", &to);
    handle("target", sim.entity_registry().find(target));

    // Moho's argument order: the weapon called on is the source.
    REQUIRE(lua.do_string("moho.weapon_methods.SetTargetEntity(from, target)").ok());
    CHECK(from.target_entity_id == target);
    CHECK(from.aim_spot_target == target); // picked now, not at its next update
    CHECK((from.current_aim_spot() == 0 || from.current_aim_spot() == 1));
    to.shots_at_target = 4;
    REQUIRE(lua.do_string("moho.weapon_methods.TransferTarget(from, to)").ok());
    CHECK(to.target_entity_id == target);
    CHECK(to.aim_spot_target == target);
    CHECK(to.shots_at_target == 0);
    CHECK(from.target_entity_id == target); // the source keeps its own

    // A ground target goes across as a ground target.
    from.set_target_ground({4, 0, 4});
    REQUIRE(lua.do_string("moho.weapon_methods.TransferTarget(from, to)").ok());
    CHECK(to.target_entity_id == 0);
    CHECK(to.has_ground_target);
    CHECK(same(to.ground_target, {4, 0, 4}));
}
