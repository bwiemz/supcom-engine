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
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_random.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <set>
#include <vector>

using osc::f32;
using osc::i32;
using osc::sim::BoneData;
using osc::sim::BoneInfo;
using osc::sim::EntityId;
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

EntityId add_unit(SimState& sim, int army, const Vector3& at, const BoneData* bones) {
    auto u = std::make_unique<Unit>();
    u->set_army(army);
    u->set_position(at);
    u->set_size_y(2.0f);
    if (bones) u->set_bone_data(bones);
    return sim.entity_registry().register_entity(std::move(u));
}

Unit& unit(SimState& sim, EntityId id) {
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

TEST_CASE("On FAF's data a unit's centre target point is its collision box's, where it stands",
          "[targetpoints]") {
    // FA-Binary-Patches hooks/CollisionOffsetFix.hook: Moho's GetTargetPoint
    // at a negative index is SizeY/2 above where the unit moves to this tick;
    // FAF's exe gives Entity::GetBoneWorldTransform(-1), the blueprint's
    // CollisionOffset with SizeY/2 up, turned with the unit where it stands
    LuaGuard g;
    SimState sim(g.L, nullptr);
    Unit& u = unit(sim, add_unit(sim, 0, {10, 0, 20}, nullptr));
    osc::sim::CollisionShape box;
    box.type = osc::sim::CollisionShapeType::BOX;
    box.cx = 0.5f;
    box.cy = 1.0f + 1.0f; // CollisionOffsetY 1, SizeY 2
    box.sx = box.sy = box.sz = 1.0f;
    u.set_default_collision_shape(box);
    u.place_now();
    u.set_position({12, 0, 20}); // moving on this tick
    CHECK(same(u.target_point(-1), {12, 1, 20}));
    sim.set_faf_exe(true);
    CHECK(same(u.target_point(-1), {10.5f, 2, 20}));
    CHECK(same(u.target_point(0), {12, 1, 20})); // an index past none: Moho's still
    u.set_collision_shape({});                   // a script's shape doesn't matter
    CHECK(same(u.target_point(-1), {10.5f, 2, 20}));
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
    const EntityId shooter = add_unit(sim, 0, {0, 0, 0}, nullptr);
    const EntityId target = add_unit(sim, 1, {0, 0, 30}, &bones);
    Weapon w;
    w.max_range = 40.0f;
    w.damage = 10.0f;
    w.muzzle_velocity = 20.0f;
    w.lead_target = false;

    // Each time the target changes it picks again; until then, the centre.
    std::set<i32> seen;
    for (int i = 0; i < 32; ++i) {
        w.set_target_entity(EntityId());
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
    const EntityId plain = add_unit(sim, 1, {5, 0, 30}, nullptr);
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
    const EntityId shooter = add_unit(sim, 0, {10, 5, 10}, nullptr);
    const auto on_bed = [&](const BoneData* bones) {
        const EntityId id = add_unit(sim, 1, {20, 0, 20}, bones);
        unit(sim, id).set_layer("Seabed");
        return id;
    };
    const EntityId wading = on_bed(&wader);
    const EntityId sunk = on_bed(&under);
    const EntityId towering = on_bed(&tall);
    const EntityId boneless = on_bed(nullptr);
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
        gun.set_target_entity(EntityId());
        gun.pick_aim_spot(sim.entity_registry(), &sim);
        gun.set_target_entity(wading);
        gun.pick_aim_spot(sim.entity_registry(), &sim);
        CHECK(gun.current_aim_spot() == 1);
        torpedo.set_target_entity(EntityId());
        torpedo.pick_aim_spot(sim.entity_registry(), &sim);
        torpedo.set_target_entity(wading);
        torpedo.pick_aim_spot(sim.entity_registry(), &sim);
        CHECK(torpedo.current_aim_spot() == 0);
    }
}

TEST_CASE("An attack on a unit on the seabed draws for a target point each time it asks whether "
          "a weapon can hit the unit",
          "[targetpoints][attack]") {
    struct Case {
        bool bones;
        const char* layer;
        int first;
        int second;
    };
    for (const Case c : {Case{true, "Seabed", 7, 3}, Case{false, "Seabed", 0, 0},
                         Case{true, "Land", 0, 0}}) {
        CAPTURE(c.bones, c.layer);
        LuaGuard g;
        SimState sim(g.L, nullptr);
        sim.set_terrain(sea());
        two_armies(sim);
        const BoneData wader = walker_bones(3.0f, 8.0f);
        auto shooter = std::make_unique<Unit>();
        shooter->set_army(0);
        shooter->set_motion_type("RULEUMT_Land");
        shooter->set_max_speed(5.0f);
        shooter->set_position({10, 5, 10});
        auto gun = std::make_unique<Weapon>();
        gun->above_water_targets_only = true;
        gun->max_range = 10.0f;
        shooter->add_weapon(std::move(gun));
        Unit* u = shooter.get();
        sim.entity_registry().register_entity(std::move(shooter));
        const EntityId target = add_unit(sim, 1, {50, 0, 50}, c.bones ? &wader : nullptr);
        unit(sim, target).set_layer(c.layer);
        osc::sim::UnitCommand attack;
        attack.type = osc::sim::CommandType::Attack;
        attack.target_id = target;
        attack.target_pos = unit(sim, target).position();
        u->push_command(attack, true);
        int draws = 0;
        sim.entity_registry().sim_random().set_draw_hook(
            +[](void* n, osc::u64) { ++*static_cast<int*>(n); }, &draws);
        sim.tick();
        CHECK(draws == c.first);
        draws = 0;
        sim.tick();
        CHECK(draws == c.second);
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
    const EntityId target = add_unit(sim, 1, {0, 0, 30}, &bones);
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
    CHECK_FALSE(to.target_entity_id);
    CHECK(to.has_ground_target);
    CHECK(same(to.ground_target, {4, 0, 4}));
}

TEST_CASE("SetTargetEntity takes only a target on the weapon's layers", "[targetpoints][lua]") {
    osc::lua::LuaState lua;
    SimState sim(lua.raw(), nullptr);
    osc::lua::register_sim_bindings(lua, sim);
    osc::lua::register_moho_bindings(lua, sim);
    two_armies(sim);
    const EntityId target = add_unit(sim, 1, {0, 0, 10}, nullptr);
    Weapon gun;
    lua_State* L = lua.raw();
    const auto handle = [&](const char* name, void* object) {
        lua_newtable(L);
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, object);
        lua_rawset(L, -3);
        lua_setglobal(L, name);
    };
    handle("gun", &gun);
    handle("target", sim.entity_registry().find(target));

    gun.fire_target_layer_caps = osc::sim::parse_layer_caps("Sub");
    REQUIRE(lua.do_string("moho.weapon_methods.SetTargetEntity(gun, target)").ok());
    CHECK_FALSE(gun.target_entity_id);
    gun.fire_target_layer_caps = osc::sim::parse_layer_caps("None");
    REQUIRE(lua.do_string("moho.weapon_methods.SetTargetEntity(gun, target)").ok());
    CHECK_FALSE(gun.target_entity_id);
    gun.fire_target_layer_caps = osc::sim::parse_layer_caps("Land");
    REQUIRE(lua.do_string("moho.weapon_methods.SetTargetEntity(gun, target)").ok());
    CHECK(gun.target_entity_id == target);
}

TEST_CASE("An order with a unit target picks its target point as it is made, whatever the order",
          "[targetpoints]") {
    osc::lua::LuaState lua;
    SimState sim(lua.raw(), nullptr);
    osc::lua::register_sim_bindings(lua, sim);
    osc::lua::register_moho_bindings(lua, sim);
    two_armies(sim);
    sim.random().set_mt19937(true);
    sim.random().seed(1);
    const BoneData bones = walker_bones(3.0f, 8.0f);
    const EntityId worker = add_unit(sim, 1, {0, 0, 0}, nullptr);
    for (const bool boned : {true, false}) {
        CAPTURE(boned);
        const EntityId target = add_unit(sim, 1, {0, 0, 20}, boned ? &bones : nullptr);
        for (const auto type : {osc::sim::CommandType::Guard, osc::sim::CommandType::Repair,
                                osc::sim::CommandType::Capture, osc::sim::CommandType::Attack}) {
            CAPTURE(static_cast<int>(type));
            osc::sim::UnitCommand cmd;
            cmd.type = type;
            cmd.target_id = target;
            cmd.target_pos = {0, 0, 20};
            const auto before = sim.random().words_drawn();
            sim.route_command({worker}, cmd, true);
            CHECK(sim.random().words_drawn() - before == (boned ? 1u : 0u));
            const auto& queue = unit(sim, worker).command_queue();
            if (!queue.empty()) {
                const i32 point = queue.front().target_point;
                CHECK((boned ? (point == 0 || point == 1) : point == -1));
            }
        }
    }
}

TEST_CASE("A script's order picks its target's target point as it reads it, and again as its "
          "command is made",
          "[targetpoints][lua]") {
    osc::lua::LuaState lua;
    SimState sim(lua.raw(), nullptr);
    osc::lua::register_sim_bindings(lua, sim);
    osc::lua::register_moho_bindings(lua, sim);
    two_armies(sim);
    sim.random().set_mt19937(true);
    sim.random().seed(1);
    const BoneData bones = walker_bones(3.0f, 8.0f);
    const EntityId target = add_unit(sim, 1, {0, 0, 20}, &bones);
    const EntityId plain = add_unit(sim, 1, {0, 0, 30}, nullptr);
    const EntityId worker = add_unit(sim, 1, {0, 0, 0}, nullptr);
    const EntityId idle = add_unit(sim, 1, {5, 0, 0}, nullptr);
    for (const char* cap : {"RULEUCC_Guard", "RULEUCC_Repair", "RULEUCC_Attack"}) {
        unit(sim, worker).add_command_cap(cap);
    }
    lua_State* L = lua.raw();
    const auto handle = [&](const char* name, EntityId id) {
        lua_newtable(L);
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, sim.entity_registry().find(id));
        lua_rawset(L, -3);
        lua_setglobal(L, name);
    };
    handle("target", target);
    handle("plain", plain);
    handle("worker", worker);
    handle("idle", idle);
    const auto drawn = [&](const char* order) {
        const auto before = sim.random().words_drawn();
        REQUIRE(lua.do_string(order).ok());
        return sim.random().words_drawn() - before;
    };
    CHECK(drawn("IssueGuard({worker}, target)") == 2);
    CHECK(drawn("IssueRepair({worker}, target)") == 2);
    CHECK(drawn("IssueAttack({worker}, target)") == 2);
    CHECK(drawn("IssueGuard({worker}, plain)") == 0);
    // Moho's IssueGuard reads its target before it looks for units that can take the order,
    // IssueRepair only once it has them.
    CHECK(drawn("IssueGuard({idle}, target)") == 1);
    CHECK(drawn("IssueRepair({idle}, target)") == 0);
}

TEST_CASE("An order moved onto another unit picks that unit's target point; a preview draws none",
          "[targetpoints][order_edit]") {
    osc::lua::LuaState lua;
    SimState sim(lua.raw(), nullptr);
    std::vector<osc::u16> heights(129 * 129, 0);
    osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
    sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
    two_armies(sim);
    sim.random().set_mt19937(true);
    sim.random().seed(1);
    const BoneData bones = walker_bones(3.0f, 8.0f);
    const EntityId plain = add_unit(sim, 1, {10, 0, 30}, nullptr);
    const EntityId target = add_unit(sim, 1, {10, 0, 40}, &bones);
    const EntityId worker = add_unit(sim, 1, {10, 0, 10}, nullptr);
    osc::sim::UnitCommand guard;
    guard.type = osc::sim::CommandType::Guard;
    guard.target_id = plain;
    guard.command_id = 7;
    unit(sim, worker).push_command(guard, true);
    auto before = sim.random().words_drawn();
    sim.tick();
    const auto idle_tick = sim.random().words_drawn() - before;

    osc::sim::SimCallbackEntry cb;
    cb.func_name = osc::sim::kSetCommandTargetCallback;
    cb.args["Command"] = 7.0;
    cb.args["Target"] = static_cast<double>(target.key());
    cb.unit_ids = {worker};
    sim.submit_callback(cb);
    before = sim.random().words_drawn();
    const auto pending = sim.queues_with_pending();
    REQUIRE(pending.count(worker) == 1);
    CHECK(pending.at(worker).orders.front().target_id == target);
    CHECK(sim.random().words_drawn() == before);
    sim.tick();
    CHECK(sim.random().words_drawn() - before == idle_tick + 1);
    REQUIRE_FALSE(unit(sim, worker).command_queue().empty());
    const osc::sim::UnitCommand& moved = unit(sim, worker).command_queue().front();
    CHECK(moved.target_id == target);
    CHECK((moved.target_point == 0 || moved.target_point == 1));
}
