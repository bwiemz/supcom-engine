#include "sim/weapon.hpp"
#include "sim/bone_data.hpp"
#include "sim/entity_registry.hpp"
#include "sim/projectile.hpp"
#include "sim/unit.hpp"

#include <algorithm>
#include <cmath>
#include <random>
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
        if (target && !target->destroyed() && target->is_unit() &&
            !target->do_not_target()) {
            // Layer cap check on existing target
            if (fire_target_layer_caps != 0xFF &&
                !(layer_to_bit(static_cast<Unit*>(target)->layer()) & fire_target_layer_caps)) {
                target_entity_id = 0;
            } else {
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
                target_entity_id = 0;
            }
        } else {
            target_entity_id = 0; // Invalid — clear
        }
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
        if (e->do_not_target()) continue;
        // Layer cap filter
        if (fire_target_layer_caps != 0xFF &&
            !(layer_to_bit(static_cast<Unit*>(e)->layer()) & fire_target_layer_caps))
            continue;

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
    if (!target || target->destroyed() || target->do_not_target() ||
        (fire_target_layer_caps != 0xFF && target->is_unit() &&
         !(layer_to_bit(static_cast<Unit*>(target)->layer()) & fire_target_layer_caps))) {
        target_entity_id = 0;
        return false;
    }

    // Bomb drop check: only fire when directly overhead
    if (need_compute_bomb_drop) {
        f32 dx = target->position().x - owner.position().x;
        f32 dz = target->position().z - owner.position().z;
        f32 horiz_dist = std::sqrt(dx * dx + dz * dz);
        if (horiz_dist > bomb_drop_threshold)
            return false; // Not overhead yet — don't fire
    }

    // Resolve muzzle bone position for projectile spawn
    Vector3 spawn_pos = owner.position();
    if (!muzzle_bone_name.empty() && owner.bone_data()) {
        i32 bi = owner.bone_data()->find_bone(muzzle_bone_name);
        if (bi >= 0) {
            auto& bone = owner.bone_data()->bones[static_cast<size_t>(bi)];
            auto rotated = quat_rotate(owner.orientation(), bone.world_position);
            spawn_pos.x += rotated.x;
            spawn_pos.y += rotated.y;
            spawn_pos.z += rotated.z;
        }
    }

    // Calculate direction to target
    f32 dx = target->position().x - spawn_pos.x;
    f32 dz = target->position().z - spawn_pos.z;
    f32 dist = std::sqrt(dx * dx + dz * dz);
    if (dist < 0.001f) dist = 0.001f;

    f32 inv_dist = 1.0f / dist;
    Vector3 vel;
    vel.x = dx * inv_dist * muzzle_velocity;
    vel.y = 0;
    vel.z = dz * inv_dist * muzzle_velocity;

    // Apply firing randomness as angular offset to velocity direction
    if (firing_randomness > 0) {
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_real_distribution<f32> ang_dist(-firing_randomness, firing_randomness);
        f32 angle = ang_dist(rng);
        f32 c = std::cos(angle), s = std::sin(angle);
        f32 nx = vel.x * c - vel.z * s;
        f32 nz = vel.x * s + vel.z * c;
        vel.x = nx;
        vel.z = nz;
    }

    // Create projectile
    auto proj = std::make_unique<Projectile>();
    proj->set_position(spawn_pos);
    proj->set_army(owner.army());
    proj->velocity = vel;
    proj->target_entity_id = need_compute_bomb_drop ? 0 : target_entity_id;
    proj->target_position = target->position();
    proj->launcher_id = owner.entity_id();
    proj->damage_amount = damage;
    proj->damage_radius = damage_radius;
    proj->damage_type = damage_type;
    // Bombs drop from altitude so need more time; normal projectiles use flight time
    proj->lifetime = need_compute_bomb_drop
        ? 10.0f  // generous for high-altitude drops
        : (dist / muzzle_velocity) + 2.0f;

    // Set projectile blueprint for rendering
    if (!projectile_bp_id.empty()) {
        proj->set_blueprint_id(projectile_bp_id);
    }

    // Velocity-align: read from projectile blueprint, default true
    proj->velocity_align = true;
    if (L && !projectile_bp_id.empty()) {
        lua_pushstring(L, "__blueprints");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, projectile_bp_id.c_str());
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "Physics");
                lua_gettable(L, -2);
                if (lua_istable(L, -1)) {
                    lua_pushstring(L, "VelocityAlign");
                    lua_gettable(L, -2);
                    if (lua_type(L, -1) == LUA_TBOOLEAN) {
                        proj->velocity_align = lua_toboolean(L, -1) != 0;
                    }
                    lua_pop(L, 1); // VelocityAlign

                    // Read ballistic acceleration (gravity) from projectile bp
                    lua_pushstring(L, "UseGravity");
                    lua_gettable(L, -2);
                    bool use_gravity = lua_isboolean(L, -1) && lua_toboolean(L, -1);
                    lua_pop(L, 1); // UseGravity

                    if (use_gravity) {
                        // Default FA gravity is -4.9
                        proj->ballistic_accel = -4.9f;
                    }

                    lua_pushstring(L, "Acceleration");
                    lua_gettable(L, -2);
                    if (lua_isnumber(L, -1)) {
                        proj->acceleration = static_cast<f32>(lua_tonumber(L, -1));
                    }
                    lua_pop(L, 1); // Acceleration

                    lua_pushstring(L, "MaxSpeed");
                    lua_gettable(L, -2);
                    if (lua_isnumber(L, -1)) {
                        proj->max_speed = static_cast<f32>(lua_tonumber(L, -1));
                    }
                    lua_pop(L, 1); // MaxSpeed

                    // Read TrackTarget
                    lua_pushstring(L, "TrackTarget");
                    lua_gettable(L, -2);
                    if (lua_type(L, -1) == LUA_TBOOLEAN) {
                        proj->tracking = lua_toboolean(L, -1) != 0;
                    }
                    lua_pop(L, 1);

                    // Read TurnRate (degrees/sec for homing)
                    lua_pushstring(L, "TurnRate");
                    lua_gettable(L, -2);
                    if (lua_isnumber(L, -1)) {
                        proj->turn_rate = static_cast<f32>(lua_tonumber(L, -1));
                    }
                    lua_pop(L, 1);

                    // Read StayUnderwater
                    lua_pushstring(L, "StayUnderwater");
                    lua_gettable(L, -2);
                    if (lua_type(L, -1) == LUA_TBOOLEAN) {
                        proj->stay_underwater = lua_toboolean(L, -1) != 0;
                    }
                    lua_pop(L, 1);
                }
                lua_pop(L, 1); // Physics
            }
            lua_pop(L, 1); // bp table
        }
        lua_pop(L, 1); // __blueprints
    }
    f32 heading = std::atan2(vel.x, vel.z);
    proj->set_orientation(euler_to_quat(heading, 0.0f, 0.0f));

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
