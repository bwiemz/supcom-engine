#include "sim/prop_collision.hpp"
#include "core/test_status.hpp"
#include "sim/collision.hpp"
#include "sim/entity_registry.hpp"
#include "sim/prop.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lua.h>
}

#include <spdlog/spdlog.h>

#include <cmath>
#include <string>
#include <vector>

namespace osc::sim {

namespace {

void call_on_collision(lua_State* L, Prop& prop, const Unit& unit, const BoxContact& contact) {
    if (prop.lua_table_ref() < 0) {
        return;
    }
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, prop.lua_table_ref());
    const int self = lua_gettop(L);
    lua_pushstring(L, "OnCollision");
    lua_gettable(L, self);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, self);
        if (unit.lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, unit.lua_table_ref());
        } else {
            lua_pushnil(L);
        }
        lua_pushnumber(L, contact.normal.x);
        lua_pushnumber(L, contact.normal.y);
        lua_pushnumber(L, contact.normal.z);
        lua_pushnumber(L, contact.depth);
        if (lua_pcall(L, 6, 0, 0) != 0) {
            const char* err = lua_tostring(L, -1);
            const std::string message =
                std::string("OnCollision error: ") + (err ? err : "(unknown)");
            spdlog::warn("{}", message);
            if (test_status::count_lua_failures()) {
                test_status::record_failure(message);
            }
        }
    }
    lua_settop(L, top);
}

} // namespace

void collide_with_props(Unit& unit, EntityRegistry& registry, lua_State* L, u32 tick) {
    if (!L || unit.entity_id() % 5 != tick % 5 || !(unit.size_x() * unit.size_z() > 0.2f)) {
        return;
    }
    const Vector3& v = unit.velocity();
    if (!((v.x * v.x + v.y * v.y + v.z * v.z) * 0.01f > 1e-6f)) {
        return;
    }
    if (unit.destroyed() || unit.is_dying() || unit.is_being_built() || unit.can_fly() ||
        unit.transport_id() != 0 || unit.parent_entity_id() != 0 || unit.layer() == "Air" ||
        unit.layer() == "Sub" || unit.has_category("NAVAL")) {
        return;
    }
    const OrientedBox box =
        oriented_box(unit.position(), unit.orientation(),
                     {unit.size_x() * 0.5f, unit.size_y(), unit.size_z() * 0.5f});
    f32 reach_x = 0;
    f32 reach_z = 0;
    for (size_t i = 0; i < 3; ++i) {
        reach_x += std::abs(box.axis[i].x) * box.extent[i];
        reach_z += std::abs(box.axis[i].z) * box.extent[i];
    }
    std::vector<Entity*> near;
    registry.props_touching(box.centre.x - reach_x, box.centre.z - reach_z, box.centre.x + reach_x,
                            box.centre.z + reach_z, near);
    for (Entity* e : near) {
        if (e->destroyed()) {
            continue;
        }
        const auto shape = collision_box(*e);
        if (!shape) {
            continue;
        }
        const auto contact = box_contact(*shape, box);
        if (!contact || contact->depth < 0.001f) {
            continue;
        }
        call_on_collision(L, static_cast<Prop&>(*e), unit, *contact);
        if (unit.destroyed()) {
            return;
        }
    }
}

} // namespace osc::sim
