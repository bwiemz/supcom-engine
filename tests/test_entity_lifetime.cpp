// What an entity owns outside the registry must be released however it is
// removed: its structure footprint on the pathfinding grid, and (when it is
// freed at the end of the tick) its Lua table's _c_object pointer back to the
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
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <memory>
#include <vector>

using osc::map::CellPassability;
using osc::map::Heightmap;
using osc::map::PathfindingGrid;
using osc::sim::SimState;
using osc::sim::Unit;

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
    const osc::u32 id = factory->entity_id();
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
    const osc::u32 id = sim.entity_registry().register_entity(std::move(u));

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
    u->set_position({32.0f, 40.0f, 32.0f});
    u->set_current_altitude(40.0f);
    u->set_current_airspeed(10.0f);
    auto* plane = u.get();
    const osc::u32 id = sim.entity_registry().register_entity(std::move(u));
    const size_t before = sim.entity_registry().count();

    plane->begin_dying(); // killed in flight; with no script it goes on landing
    for (int t = 0; t < 300 && sim.entity_registry().find(id); ++t) sim.tick();

    CHECK(sim.entity_registry().find(id) == nullptr); // impacted and removed
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
    p->lifetime = 0.05f;
    auto* raw = p.get();
    const osc::u32 id = sim.entity_registry().register_entity(std::move(p));
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, raw);
    lua_rawset(L, -3);
    lua_pushvalue(L, -1);
    lua_setglobal(L, "proj_table");
    raw->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));

    raw->update(0.1, sim.entity_registry(), L, nullptr); // lifetime runs out
    REQUIRE(sim.entity_registry().find(id) == nullptr);

    sim.tick(); // freed at the tick's end, its handle cut first
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
    const osc::u32 id = registry.register_entity(std::move(u));

    registry.unregister_entity(id);
    CHECK(registry.find(id) == nullptr); // gone for every lookup at once
    CHECK_FALSE(raw->in_registry());     // ...but still safe to touch
    CHECK(raw->entity_id() == id);

    registry.collect_garbage();
    CHECK(registry.count() == 0);
}

TEST_CASE("attached entities take their parent's pose, in any order", "[sim][attach]") {
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
    sim.follow_attachments();
    CHECK(b->position().x == 100.0f);
    CHECK(a->position().x == 50.0f); // b's pose before the step: one tick per link
    sim.follow_attachments();
    CHECK(a->position().x == 100.0f);

    // A parent that only turns: its children turn with it.
    const osc::sim::Quaternion turned{0.0f, 0.7071f, 0.0f, 0.7071f};
    c->set_orientation(turned);
    sim.follow_attachments();
    CHECK(b->orientation().y == turned.y);

    // A cycle swaps poses rather than depending on which goes first.
    Unit* d = spawn(1.0f);
    Unit* e = spawn(2.0f);
    d->set_parent(e->entity_id(), -1);
    e->set_parent(d->entity_id(), -1);
    sim.follow_attachments();
    CHECK(d->position().x == 2.0f);
    CHECK(e->position().x == 1.0f);
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
    sim.follow_attachments();
    CHECK(shield->position().x == Catch::Approx(10.0f).margin(1e-4));
    CHECK(shield->position().y == Catch::Approx(1.0f).margin(1e-4));
    CHECK(shield->position().z == Catch::Approx(19.0f).margin(1e-4));
    CHECK(shield->orientation().y == Catch::Approx(quarter.y));

    // A bone: its place in the world (model units times the scale), and the
    // offset along its turn
    osc::sim::BoneData bones;
    bones.model_scale = 2.0f;
    osc::sim::BoneInfo muzzle;
    muzzle.world_position = {0.0f, 3.0f, 1.0f};
    muzzle.world_rotation = quarter;
    bones.bones.push_back(muzzle);
    unit->set_bone_data(&bones);
    shield->set_parent(unit->entity_id(), 0);
    shield->set_parent_offset({1.0f, 0.0f, 0.0f});
    sim.follow_attachments();
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
    sim.follow_attachments();
    CHECK(script->position().x == Catch::Approx(11.0f).margin(1e-4));
    CHECK(script->position().y == Catch::Approx(6.0f).margin(1e-4));
    CHECK(script->position().z == Catch::Approx(18.0f).margin(1e-4));
}
