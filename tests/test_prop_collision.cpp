#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "lua/lua_state.hpp"
#include "sim/collision.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/prop.hpp"
#include "sim/prop_collision.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <memory>
#include <vector>

using namespace osc;
using namespace osc::sim;
using Catch::Matchers::WithinAbs;

namespace {

const Quaternion kUpright{0, 0, 0, 1};

struct Scene {
    lua::LuaState lua;
    EntityRegistry registry;
    Unit* unit = nullptr;
    Prop* tree = nullptr;

    Scene(f32 size_x, f32 size_z) {
        lua_State* L = lua.raw();
        auto u = std::make_unique<Unit>();
        u->set_size_xz(size_x, size_z);
        u->set_size_y(0.5f);
        u->set_velocity({2, 0, 0});
        lua_newtable(L);
        u->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
        unit = u.get();
        registry.register_entity(std::move(u));

        auto p = std::make_unique<Prop>();
        p->set_position({0.9f, 0, 0});
        CollisionShape shape;
        shape.type = CollisionShapeType::BOX;
        shape.cy = 1;
        shape.sx = 0.5f;
        shape.sy = 1;
        shape.sz = 0.5f;
        p->set_default_collision_shape(shape);
        REQUIRE(lua.do_string("calls = {} "
                              "tree = {OnCollision = function(self, other, nx, ny, nz, depth) "
                              "table.insert(calls, {other = other, nx = nx, ny = ny, nz = nz, "
                              "depth = depth}) end}")
                    .ok());
        lua_getglobal(L, "tree");
        p->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
        tree = p.get();
        registry.register_entity(std::move(p));
        lua_rawgeti(L, LUA_REGISTRYINDEX, unit->lua_table_ref());
        lua_setglobal(L, "unit");
    }

    int calls_after(u32 first_tick, u32 ticks) {
        for (u32 t = first_tick; t < first_tick + ticks; ++t) {
            collide_with_props(*unit, registry, lua.raw(), t);
        }
        lua_State* L = lua.raw();
        lua_getglobal(L, "calls");
        const int n = luaL_getn(L, -1);
        lua_pop(L, 1);
        return n;
    }
};

} // namespace

TEST_CASE("Two boxes meet along their least overlap, the normal toward the shape", "[collision]") {
    const OrientedBox unit = oriented_box({0, 0, 0}, kUpright, {0.7f, 1, 0.7f});
    const auto east = box_contact(oriented_box({1, 0, 0}, kUpright, {0.5f, 0.5f, 0.5f}), unit);
    REQUIRE(east);
    CHECK_THAT(east->depth, WithinAbs(0.2, 1e-6));
    CHECK(east->normal.x == 1.0f);
    CHECK(east->normal.y == 0.0f);
    CHECK(east->normal.z == 0.0f);

    const auto west = box_contact(oriented_box({-1, 0, 0}, kUpright, {0.5f, 0.5f, 0.5f}), unit);
    REQUIRE(west);
    CHECK(west->normal.x == -1.0f);

    CHECK_FALSE(box_contact(oriented_box({1.3f, 0, 0}, kUpright, {0.5f, 0.5f, 0.5f}), unit));
}

TEST_CASE("A moving unit's box calls a prop's OnCollision on the ticks its id matches mod 5",
          "[collision][lua]") {
    Scene scene(1, 1);
    const u32 id = scene.unit->entity_id();
    CHECK(scene.calls_after(id + 1, 4) == 0);
    CHECK(scene.calls_after(id + 5, 1) == 1);
    CHECK(scene.lua
              .do_string("local c = calls[1] "
                         "assert(c.other == unit, 'other') "
                         "assert(c.nx == 1 and c.ny == 0 and c.nz == 0, 'normal') "
                         "assert(math.abs(c.depth - 0.1) < 1e-5, 'depth ' .. c.depth)")
              .ok());
}

TEST_CASE("Only a unit over 0.2 in SizeX by SizeZ, and moving, runs into props",
          "[collision][lua]") {
    Scene small(0.4f, 0.5f);
    CHECK(small.calls_after(0, 10) == 0);

    Scene still(1, 1);
    still.unit->set_velocity({0.009f, 0, 0});
    CHECK(still.calls_after(0, 10) == 0);
}

TEST_CASE("The prop grid finds a prop where it stands, after it moves, until it goes",
          "[collision]") {
    EntityRegistry registry;
    registry.init_spatial_grid(64, 64);
    auto p = std::make_unique<Prop>();
    p->set_position({10, 0, 10});
    CollisionShape shape;
    shape.type = CollisionShapeType::BOX;
    shape.sx = 0.5f;
    shape.sy = 0.5f;
    shape.sz = 0.5f;
    p->set_default_collision_shape(shape);
    Prop* prop = p.get();
    const u32 id = registry.register_entity(std::move(p));
    std::vector<Entity*> near;

    registry.props_touching(10.6f, 10, 11, 10.2f, near);
    CHECK(near == std::vector<Entity*>{prop});
    registry.props_touching(30, 30, 31, 31, near);
    CHECK(near.empty());

    prop->set_position({30.5f, 0, 30.5f});
    registry.props_touching(30, 30, 31, 31, near);
    CHECK(near == std::vector<Entity*>{prop});
    registry.props_touching(10, 10, 11, 11, near);
    CHECK(near.empty());

    registry.unregister_entity(id);
    registry.props_touching(30, 30, 31, 31, near);
    CHECK(near.empty());
}
