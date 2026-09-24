#include "sim/projectile.hpp"
#include "core/dmath.hpp"
#include "core/test_status.hpp"
#include "sim/entity_registry.hpp"
#include "sim/unit.hpp"
#include "map/terrain.hpp"

#include <algorithm>
#include <cmath>
#include <string>
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

void Projectile::update(f64 dt, EntityRegistry& registry, lua_State* L,
                         const map::Terrain* terrain) {
    if (destroyed()) return;

    // Tick lifetime
    lifetime -= static_cast<f32>(dt);
    if (lifetime <= 0) {
        mark_destroyed();
        // The unregister hook nulls the Lua table's _c_object and releases
        // its ref; releasing the ref here first left scripts holding a
        // dangling pointer (UEF build-effect projectiles are destroyed from
        // Lua after they expire).
        registry.unregister_entity(entity_id());
        return;
    }
    if (impacted) return;

    // Apply ballistic acceleration (gravity)
    if (ballistic_accel != 0) {
        velocity.y += ballistic_accel * static_cast<f32>(dt);
    }

    // Apply linear acceleration capped at max_speed
    if (acceleration > 0 && max_speed > 0) {
        f32 spd = std::sqrt(velocity.x * velocity.x +
                            velocity.y * velocity.y +
                            velocity.z * velocity.z);
        if (spd > 0 && spd < max_speed) {
            f32 new_spd = std::min(max_speed,
                                    spd + acceleration * static_cast<f32>(dt));
            f32 scale = new_spd / spd;
            velocity.x *= scale;
            velocity.y *= scale;
            velocity.z *= scale;
        }
    }

    // Homing/tracking: steer velocity toward target
    if (tracking && target_entity_id > 0) {
        auto* target = registry.find(target_entity_id);
        if (target && !target->destroyed()) {
            auto pos = position();
            f32 tx = target->position().x - pos.x;
            f32 ty = target->position().y - pos.y;
            f32 tz = target->position().z - pos.z;
            f32 to_len = std::sqrt(tx * tx + ty * ty + tz * tz);
            if (to_len > 0.01f) {
                f32 spd = std::sqrt(velocity.x * velocity.x +
                                    velocity.y * velocity.y +
                                    velocity.z * velocity.z);
                if (spd < 0.01f) spd = max_speed > 0 ? max_speed : 1.0f;

                // Desired velocity: direction to target * current speed
                f32 inv_len = 1.0f / to_len;
                f32 dx = tx * inv_len * spd;
                f32 dy = ty * inv_len * spd;
                f32 dz = tz * inv_len * spd;

                // Turn rate: degrees/sec -> radians/sec
                f32 turn_rad = turn_rate * 3.14159265f / 180.0f;
                f32 max_turn = turn_rad * static_cast<f32>(dt);

                // Angle between current velocity and desired
                f32 dot = (velocity.x * dx + velocity.y * dy + velocity.z * dz) / (spd * spd);
                dot = std::clamp(dot, -1.0f, 1.0f);
                f32 angle = osc::dmath::acos(dot);

                if (angle > 0.001f) {
                    f32 t = std::min(1.0f, max_turn / angle);
                    velocity.x += (dx - velocity.x) * t;
                    velocity.y += (dy - velocity.y) * t;
                    velocity.z += (dz - velocity.z) * t;

                    // Normalize to maintain speed
                    f32 new_spd = std::sqrt(velocity.x * velocity.x +
                                            velocity.y * velocity.y +
                                            velocity.z * velocity.z);
                    if (new_spd > 0.01f) {
                        f32 scale = spd / new_spd;
                        velocity.x *= scale;
                        velocity.y *= scale;
                        velocity.z *= scale;
                    }
                }
            }
        }
    }

    // Move. Gravity was added to the velocity above; taking half of it back
    // keeps the fall on the parabola an arc's launch angle was solved for.
    auto pos = position();
    const auto step_dt = static_cast<f32>(dt);
    pos.x += velocity.x * step_dt;
    pos.y += velocity.y * step_dt - 0.5f * ballistic_accel * step_dt * step_dt;
    pos.z += velocity.z * step_dt;
    set_position(pos);

    // SetScaleVelocity: an effect that grows or shrinks as it flies.
    if (scale_velocity.x != 0 || scale_velocity.y != 0 || scale_velocity.z != 0) {
        const auto grown = [dt](f32 s, f32 v) {
            return std::max(0.0f, s + v * static_cast<f32>(dt));
        };
        set_scale(grown(scale_x(), scale_velocity.x), grown(scale_y(), scale_velocity.y),
                  grown(scale_z(), scale_velocity.z));
    }

    // Torpedo/underwater projectile: clamp Y to water surface
    if (stay_underwater && terrain) {
        if (pos.y > terrain->water_elevation()) {
            pos.y = terrain->water_elevation();
            set_position(pos);
        }
    }

    // Velocity-align orientation for rendering
    if (velocity_align) {
        f32 spd_xz = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
        if (spd_xz > 0.001f || std::abs(velocity.y) > 0.001f) {
            f32 heading = osc::dmath::atan2(velocity.x, velocity.z);
            f32 pitch = osc::dmath::atan2(-velocity.y, spd_xz);
            set_orientation(euler_to_quat(heading, pitch, 0.0f));
        }
    }

    f32 speed = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
    f32 step = speed * static_cast<f32>(dt);

    // Crossing the water's surface tells the script: a torpedo dropped from
    // the air starts tracking as it enters. One that SetDestroyOnWater ends
    // there.
    if (terrain && terrain->has_water()) {
        const bool wet = pos.y < terrain->water_elevation() &&
                         terrain->get_terrain_height(pos.x, pos.z) < terrain->water_elevation();
        if (wet != in_water) {
            in_water = wet;
            if (wet && destroy_on_water) {
                pos.y = terrain->water_elevation(); // it hit the surface
                set_position(pos);
                on_impact(L, nullptr, registry, terrain, "Water");
                return;
            }
            if (!call_script(L, registry, wet ? "OnEnterWater" : "OnExitWater")) return;
        }
    }

    // Air bursts: flak detonates on reaching its target's height above the
    // surface (DetonatesAtTargetHeight sets it); an artillery shell, once it
    // has risen above its burst height, on coming back down past it.
    {
        const f32 height = pos.y - (terrain ? terrain->get_surface_height(pos.x, pos.z) : 0.0f);
        if (detonate_below_height > 0 && height > detonate_below_height) burst_armed = true;
        if ((detonate_above_height > 0 && height >= detonate_above_height) ||
            (burst_armed && height <= detonate_below_height)) {
            on_impact(L, nullptr, registry, terrain, "Air");
            return;
        }
    }

    // Check collision with target entity
    if (target_entity_id > 0) {
        auto* target = registry.find(target_entity_id);
        if (target && !target->destroyed()) {
            f32 dx = target->position().x - pos.x;
            f32 dz = target->position().z - pos.z;
            f32 dist = std::sqrt(dx * dx + dz * dz);
            if (dist < step + HIT_RADIUS) {
                on_impact(L, target, registry, terrain);
                return;
            }
        } else {
            // Its target is gone: the script decides (a homing missile's
            // OnLostTarget shortens its lifetime). It flies on to where the
            // target was.
            target_entity_id = 0;
            if (!call_script(L, registry, "OnLostTarget")) return;
        }
    }

    // Check if reached target position (ground impact)
    {
        f32 dx = target_position.x - pos.x;
        f32 dz = target_position.z - pos.z;
        f32 dist = std::sqrt(dx * dx + dz * dz);
        if (dist < step + HIT_RADIUS) {
            on_impact(L, nullptr, registry, terrain);
            return;
        }
    }
}

const char* Projectile::impact_type(const Entity* target, const map::Terrain* terrain) const {
    const Vector3 pos = position();
    const bool wet = terrain && terrain->has_water();
    const bool under = wet && (stay_underwater || pos.y < terrain->water_elevation() - 0.5f);
    if (target) {
        if (target->is_unit()) {
            const std::string& layer = static_cast<const Unit*>(target)->layer();
            if (layer == "Air") return "UnitAir";
            if (layer == "Sub" || layer == "Seabed") return "UnitUnderwater";
            return "Unit";
        }
        if (target->is_prop()) return "Prop";
        if (target->is_shield()) return "Shield";
        if (target->is_projectile()) return under ? "ProjectileUnderwater" : "Projectile";
        return "Unit";
    }
    if (wet && terrain->get_terrain_height(pos.x, pos.z) < terrain->water_elevation())
        return under ? "Underwater" : "Water";
    return "Terrain";
}

bool Projectile::call_script(lua_State* L, EntityRegistry& registry, const char* method) {
    const u32 id = entity_id();
    if (!L || lua_table_ref() < 0) return true;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
    lua_pushstring(L, method);
    lua_gettable(L, -2);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, top + 1);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            const char* err = lua_tostring(L, -1);
            const std::string message =
                std::string("Projectile ") + method + " error: " + (err ? err : "(unknown)");
            spdlog::warn("{}", message);
            if (test_status::count_lua_failures()) test_status::record_failure(message);
        }
    }
    lua_settop(L, top);
    const Entity* self = registry.find(id);
    return self && !self->destroyed();
}

void Projectile::on_impact(lua_State* L, Entity* target, EntityRegistry& registry,
                           const map::Terrain* terrain, const char* type_override) {
    if (!L) {
        mark_destroyed();
        registry.unregister_entity(entity_id());
        return;
    }
    impacted = true;
    const u32 id = entity_id();
    const u32 target_id = target ? target->entity_id() : 0;
    const char* type = type_override ? type_override : impact_type(target, terrain);

    // Its script's OnImpact plays the impact out, as in retail: damage from
    // the DamageData its weapon passed (friendly fire, damage over time and
    // buffs included), effects chosen by the terrain, sound, and its own
    // destruction. A projectile no weapon passed damage to keeps the
    // engine's damage, so every hit counts once.
    bool scripted = false;
    bool script_damages = false;
    if (lua_table_ref() >= 0) {
        const int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
        lua_pushstring(L, "OnImpact");
        lua_gettable(L, -2);
        scripted = lua_isfunction(L, -1);
        lua_pop(L, 1);
        lua_pushstring(L, "DamageData");
        lua_gettable(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "DamageAmount");
            lua_rawget(L, -2);
            script_damages = scripted && lua_isnumber(L, -1) && lua_tonumber(L, -1) > 0;
            lua_pop(L, 1);
        }
        lua_settop(L, top);
    }
    if (!script_damages) deal_engine_damage(L, target, registry);

    if (scripted) {
        Entity* self = registry.find(id);
        if (!self || self->destroyed()) return;
        const int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, self->lua_table_ref());
        lua_pushstring(L, "OnImpact");
        lua_gettable(L, -2);
        lua_pushvalue(L, top + 1);
        lua_pushstring(L, type);
        Entity* hit = target_id ? registry.find(target_id) : nullptr;
        if (hit && !hit->destroyed() && hit->lua_table_ref() >= 0)
            lua_rawgeti(L, LUA_REGISTRYINDEX, hit->lua_table_ref());
        else lua_pushnil(L);
        const bool ok = lua_pcall(L, 3, 0, 0) == 0;
        if (!ok) {
            const char* err = lua_tostring(L, -1);
            const std::string message =
                std::string("Projectile OnImpact error: ") + (err ? err : "(unknown)");
            spdlog::warn("{}", message);
            if (test_status::count_lua_failures()) test_status::record_failure(message);
        }
        lua_settop(L, top);
        if (ok) return; // the script destroys it, now or after its ImpactTimeout
    }

    // No script to finish it (or it broke): it goes now.
    if (Entity* self = registry.find(id); self && !self->destroyed()) {
        self->mark_destroyed();
        registry.unregister_entity(id);
    }
}

void Projectile::deal_engine_damage(lua_State* L, Entity* target, EntityRegistry& registry) {
    auto pos = position();

    // Resolve launcher's Lua ref once (avoids stale pointer if launcher is
    // destroyed during DamageArea/Damage pcall in a future code path)
    int launcher_ref = -2; // LUA_NOREF
    if (launcher_id > 0) {
        Entity* launcher = registry.find(launcher_id);
        if (launcher && !launcher->destroyed() && launcher->lua_table_ref() >= 0)
            launcher_ref = launcher->lua_table_ref();
    }

    if (damage_radius > 0) {
        // Area damage: DamageArea(instigator, position, radius, damage,
        //                         damageType, damageFriendly)
        lua_pushstring(L, "DamageArea");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_isfunction(L, -1)) {
            // instigator
            if (launcher_ref >= 0) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, launcher_ref);
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
            if (launcher_ref >= 0) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, launcher_ref);
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
}

} // namespace osc::sim
