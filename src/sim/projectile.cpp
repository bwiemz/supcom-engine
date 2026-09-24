#include "sim/projectile.hpp"
#include "core/dmath.hpp"
#include "core/test_status.hpp"
#include "sim/collision.hpp"
#include "sim/entity_registry.hpp"
#include "sim/unit.hpp"
#include "map/terrain.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>
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
        // Its time is up. In flight it bursts where it is, as retail's
        // scripts expect of an 'Air' (or 'Underwater') impact; one that has
        // already impacted, lingering for its ImpactTimeout, just goes.
        if (!impacted) {
            const Vector3 at = position();
            const bool under = terrain && terrain->has_water() && at.y < terrain->water_elevation();
            on_impact(L, nullptr, registry, terrain, under ? "Underwater" : "Air");
            return;
        }
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

    // A tracking shot follows its target (the middle of it), and flies on to
    // where it last was once it is gone; with no target, to its ground target.
    if (target_entity_id > 0) {
        auto* target = registry.find(target_entity_id);
        if (target && !target->destroyed()) {
            target_position = collision_centre(*target);
            has_target_position = true;
        } else {
            // Its target is gone: the script decides (a homing missile's
            // OnLostTarget shortens its lifetime).
            target_entity_id = 0;
            if (!call_script(L, registry, "OnLostTarget")) return;
        }
    }
    if (tracking && has_target_position) {
        auto pos = position();
        f32 tx = target_position.x - pos.x;
        f32 ty = target_position.y - pos.y;
        f32 tz = target_position.z - pos.z;
        f32 to_len = std::sqrt(tx * tx + ty * ty + tz * tz);
        if (to_len > 0.01f) {
            f32 spd = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y +
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
                f32 new_spd = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y +
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

    // Move. Gravity was added to the velocity above; taking half of it back
    // keeps the fall on the parabola an arc's launch angle was solved for.
    const Vector3 from = position();
    auto pos = from;
    const auto step_dt = static_cast<f32>(dt);
    pos.x += velocity.x * step_dt;
    pos.y += velocity.y * step_dt - 0.5f * ballistic_accel * step_dt * step_dt;
    pos.z += velocity.z * step_dt;

    // Torpedo/underwater projectile: clamp Y to water surface
    if (stay_underwater && terrain && pos.y > terrain->water_elevation())
        pos.y = terrain->water_elevation();
    set_position(pos);

    // SetScaleVelocity: an effect that grows or shrinks as it flies.
    if (scale_velocity.x != 0 || scale_velocity.y != 0 || scale_velocity.z != 0) {
        const auto grown = [dt](f32 s, f32 v) {
            return std::max(0.0f, s + v * static_cast<f32>(dt));
        };
        set_scale(grown(scale_x(), scale_velocity.x), grown(scale_y(), scale_velocity.y),
                  grown(scale_z(), scale_velocity.z));
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

    if (collide(from, registry, L, terrain)) return;

    // Crossing the water's surface tells the script: a torpedo dropped from
    // the air starts tracking as it enters.
    if (terrain && terrain->has_water()) {
        const bool wet = pos.y < terrain->water_elevation() &&
                         terrain->get_terrain_height(pos.x, pos.z) < terrain->water_elevation();
        if (wet != in_water) {
            in_water = wet;
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
}

namespace {

Vector3 lerp(const Vector3& a, const Vector3& b, f32 t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

/// Where along `from` to `to` it first goes below the terrain, sampled
/// about every unit (the heightmap's spacing) and refined by halving.
std::optional<f32> terrain_crossing(const map::Terrain& terrain, const Vector3& from,
                                    const Vector3& to) {
    const auto below = [&terrain](const Vector3& p) {
        return p.y < terrain.get_terrain_height(p.x, p.z);
    };
    const f32 dx = to.x - from.x;
    const f32 dz = to.z - from.z;
    const auto samples = std::max(1, static_cast<int>(std::ceil(std::sqrt(dx * dx + dz * dz))));
    for (int i = 1; i <= samples; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(samples);
        if (!below(lerp(from, to, t))) continue;
        f32 lo = static_cast<f32>(i - 1) / static_cast<f32>(samples);
        f32 hi = t;
        for (int pass = 0; pass < 8; ++pass) {
            const f32 mid = 0.5f * (lo + hi);
            (below(lerp(from, to, mid)) ? hi : lo) = mid;
        }
        return hi;
    }
    return std::nullopt;
}

} // namespace

bool Projectile::collide(const Vector3& from, EntityRegistry& registry, lua_State* L,
                         const map::Terrain* terrain) {
    if (!collision_enabled) return false;
    const Vector3 to = position();

    // The surface: the ground, or for a shell that ends there, the water.
    f32 surface_t = 2.0f;
    const char* surface_type = nullptr;
    if (terrain && collide_surface) {
        if (const auto t = terrain_crossing(*terrain, from, to)) surface_t = *t;
        const f32 water = terrain->water_elevation();
        if (destroy_on_water && terrain->has_water() && from.y >= water && to.y < water) {
            const f32 t = (from.y - water) / (from.y - to.y);
            const Vector3 at = lerp(from, to, t);
            if (t < surface_t && terrain->get_terrain_height(at.x, at.z) < water) {
                surface_t = t;
                surface_type = "Water";
            }
        }
    }

    // A tracking shot sent to a place (a strategic missile, which skims no
    // surface, or a missile whose target died) ends on reaching it.
    if (tracking && has_target_position && target_entity_id == 0) {
        const Vector3 d{to.x - from.x, to.y - from.y, to.z - from.z};
        const Vector3 w{target_position.x - from.x, target_position.y - from.y,
                        target_position.z - from.z};
        const f32 dd = d.x * d.x + d.y * d.y + d.z * d.z;
        const f32 t =
            dd > 0 ? std::clamp((w.x * d.x + w.y * d.y + w.z * d.z) / dd, 0.0f, 1.0f) : 0.0f;
        const Vector3 near = lerp(from, to, t);
        const f32 mx = target_position.x - near.x;
        const f32 my = target_position.y - near.y;
        const f32 mz = target_position.z - near.z;
        constexpr f32 kArrived = 1.0f;
        if (mx * mx + my * my + mz * mz <= kArrived * kArrived && t < surface_t) {
            surface_t = t;
            surface_type = nullptr;
        }
    }

    // What it meets on the way, nearest first.
    if (collide_entity) {
        std::vector<u32> candidates;
        registry.collect_colliders(from.x, from.z, to.x, to.z, candidates);
        std::vector<std::pair<f32, u32>> hits;
        for (const u32 id : candidates) {
            if (id == entity_id() || id == launcher_id) continue;
            if (std::find(passed.begin(), passed.end(), id) != passed.end()) continue;
            const Entity* e = registry.find(id);
            if (!e || e->destroyed()) continue;
            if (e->is_unit()) {
                // Dying, or carried in a transport's hold: not in the way.
                const auto* u = static_cast<const Unit*>(e);
                if (u->is_dying() || u->transport_id() != 0) continue;
            }
            // A shield stops what comes in, not what goes out.
            const auto t = segment_enters(e->collision_shape(), e->position(), e->orientation(),
                                          from, to, !e->is_shield());
            if (t && *t < surface_t) hits.emplace_back(*t, id);
        }
        std::sort(hits.begin(), hits.end());
        for (const auto& [t, id] : hits) {
            Entity* e = registry.find(id);
            if (!e || e->destroyed()) continue;
            if (!collision_allowed(L, registry, *e)) {
                passed.push_back(id);
                continue;
            }
            // The checks ran scripts: both may be gone.
            if (!registry.find(entity_id()) || destroyed()) return true;
            e = registry.find(id);
            if (!e || e->destroyed()) continue;
            set_position(lerp(from, to, t));
            on_impact(L, e, registry, terrain);
            return true;
        }
        if (!registry.find(entity_id()) || destroyed()) return true;
    }

    if (surface_t > 1.0f) return false;
    Vector3 at = lerp(from, to, surface_t);
    if (surface_type && terrain) at.y = terrain->water_elevation(); // on the surface
    set_position(at);
    on_impact(L, nullptr, registry, terrain, surface_type);
    return true;
}

bool Projectile::collision_allowed(lua_State* L, EntityRegistry& registry, Entity& other) {
    if (!L) return true;
    const u32 self_id = entity_id();
    const u32 other_id = other.entity_id();
    // Each side's OnCollisionCheck(self, other); a missing one allows it.
    const auto check = [&](u32 self, u32 with) {
        const Entity* a = registry.find(self);
        const Entity* b = registry.find(with);
        if (!a || !b || a->destroyed() || b->destroyed()) return false;
        if (a->lua_table_ref() < 0 || b->lua_table_ref() < 0) return true;
        const int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, a->lua_table_ref());
        lua_pushstring(L, "OnCollisionCheck");
        lua_gettable(L, -2);
        bool allowed = true;
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, top + 1);
            lua_rawgeti(L, LUA_REGISTRYINDEX, b->lua_table_ref());
            if (lua_pcall(L, 2, 1, 0) == 0) {
                allowed = lua_toboolean(L, -1) != 0;
            } else {
                const char* err = lua_tostring(L, -1);
                const std::string message =
                    std::string("OnCollisionCheck error: ") + (err ? err : "(unknown)");
                spdlog::warn("{}", message);
                if (test_status::count_lua_failures()) test_status::record_failure(message);
            }
        }
        lua_settop(L, top);
        return allowed;
    };
    return check(self_id, other_id) && check(other_id, self_id);
}

Projectile::BlueprintPhysics Projectile::apply_blueprint_physics(lua_State* L) {
    BlueprintPhysics found;
    velocity_align = true;
    if (!L || blueprint_id().empty()) return found;
    const int top = lua_gettop(L);
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, blueprint_id().c_str());
        lua_rawget(L, -2);
    }
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "Physics");
        lua_rawget(L, -2);
    }
    if (!lua_istable(L, -1)) {
        lua_settop(L, top);
        return found;
    }
    const int physics = lua_gettop(L);
    const auto field = [&](const char* key) {
        lua_pushstring(L, key);
        lua_rawget(L, physics);
        return lua_type(L, -1);
    };
    const auto flag = [&](const char* key, bool& out) {
        if (field(key) == LUA_TBOOLEAN) out = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    };
    const auto number = [&](const char* key, f32& out) {
        if (field(key) == LUA_TNUMBER) out = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    };
    flag("VelocityAlign", velocity_align);
    number("Acceleration", acceleration);
    number("MaxSpeed", max_speed);
    flag("TrackTarget", tracking);
    number("TurnRate", turn_rate);
    flag("StayUnderwater", stay_underwater);
    // Where it ends of itself: on the water (209 retail shells), or
    // bursting at a height above the surface.
    flag("DestroyOnWater", destroy_on_water);
    number("DetonateAboveHeight", detonate_above_height);
    number("DetonateBelowHeight", detonate_below_height);
    // Strategic missiles rise from their silos through the ground.
    flag("CollideSurface", collide_surface);
    if (field("UseGravity") == LUA_TBOOLEAN) found.use_gravity = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    if (field("Lifetime") == LUA_TNUMBER) found.lifetime = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_settop(L, top);
    return found;
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
