// What an entity owns outside the registry must be released however it is
// removed: its structure footprint on the pathfinding grid, and (when it is
// freed at the end of the beat) its Lua table's _c_object pointer back to the
// C++ object.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/pathfinding_grid.hpp"
#include "map/terrain.hpp"
#include "sim/army_brain.hpp"
#include "sim/bone_data.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/projectile.hpp"
#include "sim/prop.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "support/tick_until.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <array>
#include <bit>
#include <memory>
#include <vector>

using osc::map::CellPassability;
using osc::map::Heightmap;
using osc::map::PathfindingGrid;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::test::tick_until;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

constexpr osc::u32 kMapSize = 64;

Heightmap flat_heightmap() {
    std::vector<osc::u16> heights((kMapSize + 1) * (kMapSize + 1), 1000);
    return Heightmap(kMapSize, kMapSize, 1.0f / 128.0f, std::move(heights));
}

CellPassability cell_at(const PathfindingGrid& grid, osc::f32 x, osc::f32 z) {
    osc::u32 gx = 0, gz = 0;
    grid.world_to_grid(x, z, gx, gz);
    return grid.get(gx, gz);
}

/// The _c_object of the Lua global table `name` (null if absent or cut).
void* table_object(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    lua_pushstring(L, "_c_object");
    lua_rawget(L, -2);
    void* const object = lua_touserdata(L, -1);
    lua_pop(L, 2);
    return object;
}

Unit* spawn_structure(SimState& sim, osc::f32 x, osc::f32 z) {
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_position({x, 0.0f, z});
    u->add_category("STRUCTURE");
    u->set_footprint_size(4.0f, 4.0f);
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

} // namespace

TEST_CASE("obstacle cells stay blocked while any footprint covers them",
          "[nav][m183]") {
    PathfindingGrid grid(flat_heightmap(), 0.0f, false, /*cell_size=*/2);
    grid.mark_obstacle(10.0f, 10.0f, 4.0f, 4.0f); // x 8..12
    grid.mark_obstacle(12.0f, 10.0f, 4.0f, 4.0f); // x 10..14
    REQUIRE(cell_at(grid, 11.0f, 10.0f) == CellPassability::Obstacle);

    grid.clear_obstacle(10.0f, 10.0f, 4.0f, 4.0f);
    CHECK(cell_at(grid, 9.0f, 10.0f) == CellPassability::Passable);
    CHECK(cell_at(grid, 11.0f, 10.0f) == CellPassability::Obstacle);
    CHECK(cell_at(grid, 13.0f, 10.0f) == CellPassability::Obstacle);

    grid.clear_obstacle(12.0f, 10.0f, 4.0f, 4.0f);
    CHECK(cell_at(grid, 11.0f, 10.0f) == CellPassability::Passable);
    CHECK(cell_at(grid, 13.0f, 10.0f) == CellPassability::Passable);

    // Clearing again (or clearing what was never marked) is harmless.
    grid.clear_obstacle(12.0f, 10.0f, 4.0f, 4.0f);
    CHECK(cell_at(grid, 13.0f, 10.0f) == CellPassability::Passable);
}

TEST_CASE("a footprint blocks the cells whose centres it covers", "[nav]") {
    PathfindingGrid grid(flat_heightmap(), 0.0f, false, /*cell_size=*/2);
    grid.mark_obstacle(40.0f, 40.0f, 5.0f, 5.0f); // x 37.5..42.5
    CHECK(cell_at(grid, 37.25f, 40.0f) == CellPassability::Passable);
    CHECK(cell_at(grid, 38.5f, 40.0f) == CellPassability::Obstacle);
    CHECK(cell_at(grid, 41.5f, 40.0f) == CellPassability::Obstacle);
    CHECK(cell_at(grid, 42.75f, 40.0f) == CellPassability::Passable);

    grid.mark_obstacle(20.5f, 20.5f, 0.8f, 0.8f);
    CHECK(cell_at(grid, 20.5f, 20.5f) == CellPassability::Obstacle);
    CHECK(cell_at(grid, 18.5f, 20.5f) == CellPassability::Passable);
    CHECK(cell_at(grid, 22.5f, 20.5f) == CellPassability::Passable);
}

TEST_CASE("a removed structure stops blocking paths", "[nav][m183]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_terrain(std::make_unique<osc::map::Terrain>(flat_heightmap(), 0.0f, false));
    sim.build_pathfinding_grid();
    auto& grid = *sim.pathfinding_grid();

    auto* factory = spawn_structure(sim, 20.0f, 20.0f);
    const osc::sim::EntityId id = factory->entity_id();
    sim.occupy_footprint(*factory);
    sim.occupy_footprint(*factory); // idempotent
    REQUIRE(sim.occupies_footprint(id));
    REQUIRE(cell_at(grid, 20.0f, 20.0f) == CellPassability::Obstacle);

    // Any removal path (reclaim, sacrifice, Lua Destroy) ends here.
    sim.entity_registry().unregister_entity(id);
    CHECK_FALSE(sim.occupies_footprint(id));
    CHECK(cell_at(grid, 20.0f, 20.0f) == CellPassability::Passable);
}

TEST_CASE("non-structures never occupy the grid", "[nav][m183]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_terrain(std::make_unique<osc::map::Terrain>(flat_heightmap(), 0.0f, false));
    sim.build_pathfinding_grid();

    auto u = std::make_unique<Unit>();
    u->set_position({30.0f, 0.0f, 30.0f});
    u->set_footprint_size(2.0f, 2.0f); // mobile unit: no STRUCTURE category
    auto* tank = u.get();
    sim.entity_registry().register_entity(std::move(u));
    sim.occupy_footprint(*tank);
    CHECK_FALSE(sim.occupies_footprint(tank->entity_id()));
    CHECK(cell_at(*sim.pathfinding_grid(), 30.0f, 30.0f) == CellPassability::Passable);
}

TEST_CASE("C++-side removal severs the Lua table's pointer to the entity",
          "[lifetime][m183]") {
    LuaGuard g;
    lua_State* L = g.L;
    SimState sim(L, nullptr);

    auto u = std::make_unique<Unit>();
    auto* raw = u.get();
    const osc::sim::EntityId id = sim.entity_registry().register_entity(std::move(u));

    // The unit's Lua table, as create_unit_core builds it.
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, raw);
    lua_rawset(L, -3);
    lua_pushvalue(L, -1);
    lua_setglobal(L, "unit_table"); // a script keeps a handle
    raw->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));

    // e.g. reclaim / sacrifice / projectile impact -- no entity_Destroy.
    sim.entity_registry().unregister_entity(id);
    // Moho deletes a destroyed entity at the end of the beat: until then its
    // handle still reaches it.
    CHECK(table_object(L, "unit_table") == raw);

    sim.tick();                                      // its end frees it, cutting the handle first
    CHECK(table_object(L, "unit_table") == nullptr); // stale handle reads as destroyed
}

TEST_CASE("a crashed aircraft is removed from the registry", "[lifetime][m183]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_terrain(std::make_unique<osc::map::Terrain>(flat_heightmap(), 0.0f, false));
    sim.build_pathfinding_grid();

    auto u = std::make_unique<Unit>();
    u->set_layer("Air");
    u->set_motion_type("RULEUMT_Air");
    u->set_position({32.0f, 40.0f, 32.0f});
    u->set_current_altitude(40.0f);
    u->set_current_airspeed(10.0f);
    auto* plane = u.get();
    const osc::sim::EntityId id = sim.entity_registry().register_entity(std::move(u));
    const size_t before = sim.entity_registry().count();

    plane->begin_dying(); // killed in flight; with no script it goes on landing
    tick_until([&] { sim.tick(); }, "the crashed aircraft is removed", 300,
               [&] { return sim.entity_registry().find(id) == nullptr; });

    CHECK(sim.entity_registry().count() == before - 1);
}

TEST_CASE("an expired projectile's Lua table no longer points at it",
          "[lifetime]") {
    // UEF build effects keep projectile handles in a trash bag and Destroy
    // them from Lua after the projectile's own lifetime has run out.
    LuaGuard g;
    lua_State* L = g.L;
    SimState sim(L, nullptr);

    auto p = std::make_unique<osc::sim::Projectile>();
    p->set_lifetime(0.04f, 0);
    auto* raw = p.get();
    const osc::sim::EntityId id = sim.entity_registry().register_entity(std::move(p));
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, raw);
    lua_rawset(L, -3);
    lua_pushvalue(L, -1);
    lua_setglobal(L, "proj_table");
    raw->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));

    raw->update(0.1, sim.entity_registry(), L, nullptr); // lifetime runs out
    raw->update(0.1, sim.entity_registry(), L, nullptr);
    REQUIRE(raw->destroyed());

    sim.tick(); // freed at the beat's end, its handle cut first
    CHECK(sim.entity_registry().find(id) == nullptr);
    CHECK(table_object(L, "proj_table") == nullptr);
}

TEST_CASE("an unregistered entity stays allocated until the tick's garbage collection",
          "[lifetime]") {
    // Scripts destroy a unit from inside its own callbacks (a structure
    // finishing an upgrade replaces itself) while C++ is still in that
    // unit's update; the object must outlive the call.
    osc::sim::EntityRegistry registry;
    auto u = std::make_unique<Unit>();
    auto* raw = u.get();
    const osc::sim::EntityId id = registry.register_entity(std::move(u));

    registry.unregister_entity(id);
    CHECK(registry.find(id) == nullptr); // gone for every lookup at once
    CHECK_FALSE(raw->in_registry());     // ...but still safe to touch
    CHECK(raw->entity_id() == id);

    registry.collect_garbage();
    CHECK(registry.count() == 0);
}

TEST_CASE("A chain of attached entities follows its root in the tick it moves", "[sim][attach]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    auto spawn = [&](osc::f32 x) {
        auto u = std::make_unique<Unit>();
        u->set_position({x, 0.0f, 0.0f});
        auto* p = u.get();
        sim.entity_registry().register_entity(std::move(u));
        return p;
    };
    // A chain: a on b on c.
    Unit* c = spawn(100.0f);
    Unit* b = spawn(50.0f);
    Unit* a = spawn(10.0f);
    b->set_parent(c->entity_id(), -1);
    a->set_parent(b->entity_id(), -1);
    sim.tick();
    CHECK(b->position().x == 100.0f);
    CHECK(a->position().x == 100.0f);

    // A parent that only turns: its children turn with it.
    const osc::sim::Quaternion turned{0.0f, 0.7071f, 0.0f, 0.7071f};
    c->set_orientation(turned);
    sim.tick();
    CHECK(b->orientation().y == osc::sim::quat_normalized_moho(turned).y);
    CHECK(a->orientation().y == osc::sim::quat_normalized_moho(turned).y);

    // A cycle, which Moho's AttachTo refuses, doesn't hold the walk up.
    Unit* d = spawn(1.0f);
    Unit* e = spawn(2.0f);
    d->set_parent(e->entity_id(), -1);
    e->set_parent(d->entity_id(), -1);
    sim.tick();
    CHECK(d->position().x == 2.0f);
    CHECK(e->position().x == 2.0f);
}

TEST_CASE("An attached entity sits at its parent bone, then its offset (M211k)", "[sim][attach]") {
    // Moho's CalculateAttachedTransform: the parent bone's world transform
    // (a unit's bone -1 is its centre, half its height up), then the parent
    // offset in that bone's frame, as a shield sits on its generator.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    const osc::sim::Quaternion quarter{0.0f, 0.70710678f, 0.0f, 0.70710678f}; // +x turns to -z
    auto owned = std::make_unique<Unit>();
    Unit* unit = owned.get();
    unit->set_position({10.0f, 0.0f, 20.0f});
    unit->set_orientation(quarter);
    unit->set_size_y(4.0f);
    sim.entity_registry().register_entity(std::move(owned));
    auto child_owned = std::make_unique<osc::sim::Shield>();
    osc::sim::Shield* shield = child_owned.get();
    sim.entity_registry().register_entity(std::move(child_owned));

    // Bone -1: the centre (0, 2, 0) up, then (1, -1, 0) turned to (0, -1, -1)
    shield->set_parent(unit->entity_id(), -1);
    shield->set_parent_offset({1.0f, -1.0f, 0.0f});
    sim.tick();
    CHECK(shield->position().x == Catch::Approx(10.0f).margin(1e-4));
    CHECK(shield->position().y == Catch::Approx(1.0f).margin(1e-4));
    CHECK(shield->position().z == Catch::Approx(19.0f).margin(1e-4));
    CHECK(shield->orientation().y == Catch::Approx(quarter.y));

    // A bone: its place in the world (model units times the scale), and the
    // offset along its turn
    osc::sim::BoneData bones;
    bones.model_scale = 2.0f;
    osc::sim::BoneInfo muzzle;
    muzzle.local_position = muzzle.world_position = {0.0f, 3.0f, 1.0f};
    muzzle.local_rotation = muzzle.world_rotation = quarter;
    bones.bones.push_back(muzzle);
    unit->set_bone_data(&bones);
    shield->set_parent(unit->entity_id(), 0);
    shield->set_parent_offset({1.0f, 0.0f, 0.0f});
    sim.tick();
    // (0, 6, 2) turned: (2, 6, 0); the offset turned twice: (-1, 0, 0)
    CHECK(shield->position().x == Catch::Approx(11.0f).margin(1e-4));
    CHECK(shield->position().y == Catch::Approx(6.0f).margin(1e-4));
    CHECK(shield->position().z == Catch::Approx(20.0f).margin(1e-4));
    unit->set_bone_data(nullptr);

    // Another entity's bone -1 is its own place (no half height), turned as
    // it is: the shield took the bone's turn, a quarter on a quarter, so
    // (0, 0, 2) is (0, 0, -2)
    shield->clear_parent();
    auto script_owned = std::make_unique<osc::sim::Shield>();
    osc::sim::Shield* script = script_owned.get();
    sim.entity_registry().register_entity(std::move(script_owned));
    script->set_parent(shield->entity_id(), -1);
    script->set_parent_offset({0.0f, 0.0f, 2.0f});
    sim.tick();
    CHECK(script->position().x == Catch::Approx(11.0f).margin(1e-4));
    CHECK(script->position().y == Catch::Approx(6.0f).margin(1e-4));
    CHECK(script->position().z == Catch::Approx(18.0f).margin(1e-4));
}

TEST_CASE("An attached entity's turn is made unit length, as CalculateAttachedTransform's",
          "[sim][attach]") {
    // UAC1401's root bone in its SCM, and its dome entity in retail 3599's /synclog.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    osc::sim::BoneData bones;
    osc::sim::BoneInfo root;
    root.local_rotation = root.world_rotation = {std::bit_cast<osc::f32>(0x33b504f3u), 0.0f, 0.0f,
                                                 std::bit_cast<osc::f32>(0x3f7fffffu)};
    bones.bones.push_back(root);
    auto owned = std::make_unique<Unit>();
    Unit* unit = owned.get();
    unit->set_position({253.5f, 0.0f, 245.5f});
    unit->set_bone_data(&bones);
    sim.entity_registry().register_entity(std::move(owned));
    auto dome_owned = std::make_unique<osc::sim::Shield>();
    osc::sim::Shield* dome = dome_owned.get();
    sim.entity_registry().register_entity(std::move(dome_owned));
    dome->set_parent(unit->entity_id(), 0);
    sim.tick();
    CHECK(std::bit_cast<osc::u32>(dome->orientation().w) == 0x3f800000u);
    CHECK(std::bit_cast<osc::u32>(dome->orientation().x) == 0x33b504f4u);
}

TEST_CASE("A unit's bone -1 is half its height straight up, however it is tilted",
          "[sim][attach]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    auto owned = std::make_unique<Unit>();
    Unit* unit = owned.get();
    unit->set_position({10.0f, 0.0f, 20.0f});
    unit->set_orientation({0.25881904f, 0.0f, 0.0f, 0.96592583f});
    unit->set_size_y(4.0f);
    sim.entity_registry().register_entity(std::move(owned));
    auto child_owned = std::make_unique<osc::sim::Shield>();
    osc::sim::Shield* child = child_owned.get();
    sim.entity_registry().register_entity(std::move(child_owned));
    child->set_parent(unit->entity_id(), -1);
    sim.tick();
    CHECK(child->position().x == 10.0f);
    CHECK(child->position().y == 2.0f);
    CHECK(child->position().z == 20.0f);
}

namespace {

osc::f32 bits(osc::u32 b) {
    return std::bit_cast<osc::f32>(b);
}

/// Bone `index` of a mesh, its inverse bind as CAniSkel's: offset, then turn (w, x, y, z).
osc::sim::BoneData mesh_bone(size_t index, osc::f32 scale, std::array<osc::u32, 3> at,
                             std::array<osc::u32, 4> turn) {
    osc::sim::BoneData bones;
    bones.model_scale = scale;
    bones.bones.resize(index + 1);
    bones.bones[index].inverse_bind = {
        {bits(at[0]), bits(at[1]), bits(at[2])},
        {bits(turn[1]), bits(turn[2]), bits(turn[3]), bits(turn[0])}};
    return bones;
}

template <typename T>
T* add(SimState& sim, osc::sim::Vector3 at, osc::sim::Quaternion facing = {}) {
    auto owned = std::make_unique<T>();
    T* e = owned.get();
    e->set_position(at);
    e->set_orientation(facing);
    sim.entity_registry().register_entity(std::move(owned));
    return e;
}

} // namespace

TEST_CASE("What hangs on a prop's bone sits as the original game's", "[sim][attach]") {
    // Pine06_Big_GroupA's bone 3 and Cactus01_s1's root in their SCMs; the original game's
    // GetPosition and GetOrientation of each child.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    const osc::sim::BoneData pine = mesh_bone(3, 0.08f, {0xc1c23097u, 0x41e15b03u, 0xb3cfef8au},
                                              {0x3f25f669u, 0x3f25f669u, 0x3e908f7cu, 0x3e908f7cu});
    const osc::sim::BoneData cactus =
        mesh_bone(0, 0.018f, {0x80000000u, 0x80000000u, 0x80000000u},
                  {0x3f3504f3u, 0x3f3504f3u, 0x80000000u, 0x80000000u});
    auto* group = add<osc::sim::Prop>(sim, {702.5f, 11.6523438f, 416.5f},
                                      {0.0f, 0.258819044f, 0.0f, 0.965925813f});
    group->set_bone_data(&pine);
    auto* bare = add<osc::sim::Shield>(sim, {});
    bare->set_parent(group->entity_id(), 3);
    auto* boxed = add<osc::sim::Prop>(sim, {});
    boxed->set_default_collision_shape(
        {osc::sim::CollisionShapeType::BOX, 0.0f, 0.125f, 0.0f, 0.05f, 0.125f, 0.05f});
    boxed->set_parent(group->entity_id(), 3);
    auto* rooted = add<osc::sim::Prop>(sim, {});
    rooted->set_bone_data(&cactus);
    rooted->set_parent(group->entity_id(), 3, 0);
    sim.tick();

    CHECK(bare->position().x == 703.694946f);
    CHECK(bare->position().y == 11.6523438f);
    CHECK(bare->position().z == 419.224274f);
    CHECK(bare->orientation().x == -0.699277401f);
    CHECK(bare->orientation().y == -0.104933947f);
    CHECK(bare->orientation().z == -0.104933947f);
    CHECK(bare->orientation().w == 0.699277401f);
    CHECK(boxed->position().x == 703.658264f);
    CHECK(boxed->position().y == 11.6523438f);
    CHECK(boxed->position().z == 419.343781f);
    CHECK(rooted->position().x == 703.694946f);
    CHECK(rooted->position().z == 419.224274f);
    CHECK(std::bit_cast<osc::u32>(rooted->orientation().x) == 0u);
    CHECK(rooted->orientation().y == -0.148398995f);
    CHECK(std::bit_cast<osc::u32>(rooted->orientation().z) == 0u);
    CHECK(rooted->orientation().w == 0.988927603f);
}

TEST_CASE("A prop hung by its own bone from a unit's sits as the original game's",
          "[sim][attach]") {
    // URL0001's root and Cactus01_Group1's bone 5 in their SCMs; the original game's
    // GetPosition and GetOrientation of the group.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    osc::sim::BoneData acu;
    acu.model_scale = 0.16f;
    osc::sim::BoneInfo root;
    root.local_position = root.world_position = {0.0f, bits(0x410449bau), 0.0f};
    root.local_rotation = root.world_rotation = {bits(0xb4200000u), 0.0f, 0.0f, 1.0f};
    acu.bones.push_back(root);
    const osc::sim::BoneData group_bones =
        mesh_bone(5, 0.018f, {0x3fba0a11u, 0xc16ce3f0u, 0xb29366d2u},
                  {0x3f263cf5u, 0x3f263cf4u, 0xbe8f49d3u, 0xbe8f49d3u});
    auto* unit = add<Unit>(sim, {672.5f, 18.6796875f, 346.5f});
    unit->set_bone_data(&acu);
    auto* group = add<osc::sim::Prop>(sim, {});
    group->set_bone_data(&group_bones);
    group->set_parent(unit->entity_id(), 0, 5);
    sim.tick();

    CHECK(group->position().x == 672.526184f);
    CHECK(group->position().y == 19.7360649f);
    CHECK(group->position().z == 346.5f);
    CHECK(group->orientation().x == 0.649367452f);
    CHECK(group->orientation().y == -0.279860139f);
    CHECK(group->orientation().z == -0.279860079f);
    CHECK(group->orientation().w == 0.64936775f);
}

TEST_CASE("A prop's bone in the world is its mesh's rest bone, as the original game's",
          "[sim][attach]") {
    // Cactus01_Group1's bones 1 and 5 in its SCM; the original game's GetPosition(bone).
    LuaGuard g;
    SimState sim(g.L, nullptr);
    osc::sim::BoneData bones = mesh_bone(5, 0.018f, {0x3fba0a11u, 0xc16ce3f0u, 0xb29366d2u},
                                         {0x3f263cf5u, 0x3f263cf4u, 0xbe8f49d3u, 0xbe8f49d3u});
    bones.bones[1].inverse_bind = {
        {bits(0xc02fa4b2u), bits(0xc09ea6f4u), bits(0xb2a8ea1fu)},
        {bits(0x3f326e66u), bits(0x3df401aeu), bits(0x3df401aeu), bits(0x3f326e64u)}};
    auto* group = add<osc::sim::Prop>(sim, {722.5f, 22.0722656f, 376.5f});
    group->set_bone_data(&bones);

    const osc::sim::Vector3 one = group->bone_world_transform(1).position;
    CHECK(one.x == 722.576233f);
    CHECK(one.y == 22.0722656f);
    CHECK(one.z == 376.43222f);
    const osc::sim::Vector3 five = group->bone_world_transform(5).position;
    CHECK(five.x == 722.28833f);
    CHECK(five.y == 22.0722656f);
    CHECK(five.z == 376.335999f);
}
