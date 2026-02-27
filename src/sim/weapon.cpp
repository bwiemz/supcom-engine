#include "sim/weapon.hpp"
#include "sim/entity_registry.hpp"
#include "sim/projectile.hpp"
#include "sim/unit.hpp"

#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::sim {

void Weapon::update(f64 dt, Unit& owner, EntityRegistry& registry,
                    lua_State* L) {
    if (!enabled || fire_on_death || manual_fire) return;
    if (max_range <= 0 || damage <= 0) return;
    // HoldFire (1) = don't auto-target or fire at all
    if (owner.fire_state() == 1) return;

    // Tick cooldown
    fire_cooldown = std::max(0.0f, fire_cooldown - static_cast<f32>(dt));

    // Target acquisition
    update_targeting(owner, registry);

    if (target_entity_id == 0) return;

    // Fire if cooldown expired
    if (fire_cooldown <= 0) {
        if (try_fire(owner, registry, L)) {
            fire_cooldown = (rate_of_fire > 0) ? (1.0f / rate_of_fire) : 1.0f;
        }
    }
}

void Weapon::update_targeting(Unit& owner, EntityRegistry& registry) {
    // Check if current target is still valid
    if (target_entity_id > 0) {
        auto* target = registry.find(target_entity_id);
        if (target && !target->destroyed() && target->is_unit()) {
            // Check range
            f32 dx = target->position().x - owner.position().x;
            f32 dz = target->position().z - owner.position().z;
            f32 dist2 = dx * dx + dz * dz;
            f32 max2 = max_range * max_range;
            f32 min2 = min_range * min_range;
            if (dist2 <= max2 && dist2 >= min2 &&
                target->army() != owner.army()) {
                return; // Current target still valid
            }
        }
        target_entity_id = 0; // Invalid — clear
    }

    // Find nearest enemy in range
    auto candidates = registry.collect_in_radius(
        owner.position().x, owner.position().z, max_range);

    f32 best_dist2 = max_range * max_range + 1.0f;
    u32 best_id = 0;
    f32 min2 = min_range * min_range;

    for (u32 id : candidates) {
        auto* e = registry.find(id);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        if (e->army() == owner.army() || e->army() < 0) continue;
        if (e->entity_id() == owner.entity_id()) continue;

        f32 dx = e->position().x - owner.position().x;
        f32 dz = e->position().z - owner.position().z;
        f32 dist2 = dx * dx + dz * dz;
        if (dist2 < min2) continue;
        if (dist2 < best_dist2) {
            best_dist2 = dist2;
            best_id = id;
        }
    }

    target_entity_id = best_id;
}

bool Weapon::try_fire(Unit& owner, EntityRegistry& registry,
                      lua_State* L) {
    auto* target = registry.find(target_entity_id);
    if (!target || target->destroyed()) {
        target_entity_id = 0;
        return false;
    }

    // Calculate direction to target
    f32 dx = target->position().x - owner.position().x;
    f32 dz = target->position().z - owner.position().z;
    f32 dist = std::sqrt(dx * dx + dz * dz);
    if (dist < 0.001f) dist = 0.001f;

    f32 inv_dist = 1.0f / dist;
    Vector3 vel;
    vel.x = dx * inv_dist * muzzle_velocity;
    vel.y = 0;
    vel.z = dz * inv_dist * muzzle_velocity;

    // Create projectile
    auto proj = std::make_unique<Projectile>();
    proj->set_position(owner.position());
    proj->set_army(owner.army());
    proj->velocity = vel;
    proj->target_entity_id = target_entity_id;
    proj->target_position = target->position();
    proj->launcher_id = owner.entity_id();
    proj->damage_amount = damage;
    proj->damage_radius = damage_radius;
    proj->damage_type = damage_type;
    proj->lifetime = (dist / muzzle_velocity) + 2.0f; // flight time + buffer

    u32 proj_id = registry.register_entity(std::move(proj));
    auto* proj_ptr = static_cast<Projectile*>(registry.find(proj_id));

    // Create Lua table for projectile with projectile_methods metatable
    if (L && proj_ptr) {
        lua_newtable(L);
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, proj_ptr);
        lua_rawset(L, -3);

        // Set metatable from moho.projectile_methods (via cached __osc_proj_mt)
        lua_pushstring(L, "__osc_proj_mt");
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            // Build metatable
            lua_newtable(L);
            int mt_idx = lua_gettop(L);
            lua_pushstring(L, "__index");
            lua_pushvalue(L, mt_idx);
            lua_rawset(L, mt_idx);

            // Copy from moho.projectile_methods
            lua_pushstring(L, "moho");
            lua_rawget(L, LUA_GLOBALSINDEX);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "projectile_methods");
                lua_rawget(L, -2);
                if (lua_istable(L, -1)) {
                    int src_idx = lua_gettop(L);
                    lua_pushnil(L);
                    while (lua_next(L, src_idx) != 0) {
                        lua_pushvalue(L, -2);
                        lua_pushvalue(L, -2);
                        lua_rawset(L, mt_idx);
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1); // projectile_methods
            }
            lua_pop(L, 1); // moho

            // Cache in registry
            lua_pushstring(L, "__osc_proj_mt");
            lua_pushvalue(L, mt_idx);
            lua_rawset(L, LUA_REGISTRYINDEX);
        }
        lua_setmetatable(L, -2);

        // Store Lua table ref
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        proj_ptr->set_lua_table_ref(ref);
    }

    spdlog::debug("Weapon '{}' fired projectile #{} at entity #{}",
                  label, proj_id, target_entity_id);
    return true;
}

} // namespace osc::sim
