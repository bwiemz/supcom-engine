#include "sim/projectile.hpp"
#include "sim/entity_registry.hpp"

#include <cmath>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::sim {

// Helper: push a Vector3 as {[1]=x, [2]=y, [3]=z}
static void push_vec3(lua_State* L, const Vector3& v) {
    lua_newtable(L);
    lua_pushnumber(L, 1);
    lua_pushnumber(L, v.x);
    lua_settable(L, -3);
    lua_pushnumber(L, 2);
    lua_pushnumber(L, v.y);
    lua_settable(L, -3);
    lua_pushnumber(L, 3);
    lua_pushnumber(L, v.z);
    lua_settable(L, -3);
}

void Projectile::update(f64 dt, EntityRegistry& registry, lua_State* L) {
    if (destroyed()) return;

    // Tick lifetime
    lifetime -= static_cast<f32>(dt);
    if (lifetime <= 0) {
        mark_destroyed();
        // Unregister from Lua
        if (lua_table_ref() >= 0 && L) {
            luaL_unref(L, LUA_REGISTRYINDEX, lua_table_ref());
            set_lua_table_ref(-2); // LUA_NOREF
        }
        registry.unregister_entity(entity_id());
        return;
    }

    // Move
    auto pos = position();
    pos.x += velocity.x * static_cast<f32>(dt);
    pos.y += velocity.y * static_cast<f32>(dt);
    pos.z += velocity.z * static_cast<f32>(dt);
    set_position(pos);

    f32 speed = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
    f32 step = speed * static_cast<f32>(dt);

    // Check collision with target entity
    if (target_entity_id > 0) {
        auto* target = registry.find(target_entity_id);
        if (target && !target->destroyed()) {
            f32 dx = target->position().x - pos.x;
            f32 dz = target->position().z - pos.z;
            f32 dist = std::sqrt(dx * dx + dz * dz);
            if (dist < step + HIT_RADIUS) {
                on_impact(L, target, registry);
                return;
            }
        }
        // Target destroyed — fall through to ground target check
    }

    // Check if reached target position (ground impact)
    {
        f32 dx = target_position.x - pos.x;
        f32 dz = target_position.z - pos.z;
        f32 dist = std::sqrt(dx * dx + dz * dz);
        if (dist < step + HIT_RADIUS) {
            on_impact(L, nullptr, registry);
            return;
        }
    }
}

void Projectile::on_impact(lua_State* L, Entity* target,
                           EntityRegistry& registry) {
    if (!L) {
        mark_destroyed();
        registry.unregister_entity(entity_id());
        return;
    }

    auto pos = position();

    // Find launcher entity for instigator arg
    Entity* launcher = nullptr;
    if (launcher_id > 0) {
        launcher = registry.find(launcher_id);
        if (launcher && launcher->destroyed()) launcher = nullptr;
    }

    if (damage_radius > 0) {
        // Area damage: DamageArea(instigator, position, radius, damage,
        //                         damageType, damageFriendly)
        lua_pushstring(L, "DamageArea");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_isfunction(L, -1)) {
            // instigator
            if (launcher && launcher->lua_table_ref() >= 0) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, launcher->lua_table_ref());
            } else {
                lua_pushnil(L);
            }
            // position
            push_vec3(L, pos);
            // radius
            lua_pushnumber(L, damage_radius);
            // damage
            lua_pushnumber(L, damage_amount);
            // damageType
            lua_pushstring(L, damage_type.c_str());
            // damageFriendly
            lua_pushboolean(L, 0);
            if (lua_pcall(L, 6, 0, 0) != 0) {
                spdlog::warn("Projectile DamageArea error: {}",
                             lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
    } else if (target && !target->destroyed()) {
        // Single-target damage: Damage(instigator, target, amount, vector,
        //                               damageType) — FA canonical 5-arg form
        lua_pushstring(L, "Damage");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_isfunction(L, -1)) {
            // instigator
            if (launcher && launcher->lua_table_ref() >= 0) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, launcher->lua_table_ref());
            } else {
                lua_pushnil(L);
            }
            // target
            if (target->lua_table_ref() >= 0) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
            } else {
                lua_pushnil(L);
            }
            // amount
            lua_pushnumber(L, damage_amount);
            // vector (impact position)
            push_vec3(L, pos);
            // damageType
            lua_pushstring(L, damage_type.c_str());
            if (lua_pcall(L, 5, 0, 0) != 0) {
                spdlog::warn("Projectile Damage error: {}",
                             lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
    }

    // Destroy projectile
    mark_destroyed();
    if (lua_table_ref() >= 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, lua_table_ref());
        set_lua_table_ref(-2); // LUA_NOREF
    }
    registry.unregister_entity(entity_id());
}

} // namespace osc::sim
