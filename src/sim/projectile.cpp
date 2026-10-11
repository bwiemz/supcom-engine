#include "sim/projectile.hpp"
#include "sim/sim_random.hpp"
#include "core/dmath.hpp"
#include "core/test_status.hpp"
#include "sim/collision.hpp"
#include "sim/entity_registry.hpp"
#include "sim/flight_math.hpp"
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

namespace {

constexpr f32 kDegToRad = 0.017453292f;

/// The water's height, or Moho's -10000 on a map without
f32 water_line(const map::Terrain* terrain) {
    return terrain && terrain->has_water() ? terrain->water_elevation() : -10000.0f;
}

} // namespace

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

void Projectile::update(f64 dt, EntityRegistry& registry, lua_State* L, const map::Terrain* terrain,
                        u32 tick) {
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

    // Moho's MotionTick (faf-re Projectile.cpp): a shot thrusts the way it
    // faces. One that doesn't track falls (Moho runs the ballistic step only
    // `if (!mTrackTarget)`: FAF's torpedoes say UseGravity and sank to the
    // seabed under their sub), and with VelocityAlign turns after its
    // velocity; one that tracks turns toward its target first
    // (UpdateTracking). Its speed is capped, and it moves by the mean of its
    // old and new velocity (a shell's fall stays on the parabola its launch
    // angle was solved for).
    const auto step = static_cast<f32>(dt);
    const Vector3 start_velocity = velocity;
    const Vector3 from = position();
    Quaternion facing = orientation();
    const auto thrust = [&] {
        const Vector3 ahead = forward_of(facing);
        velocity.x += ahead.x * acceleration * step;
        velocity.y += ahead.y * acceleration * step;
        velocity.z += ahead.z * acceleration * step;
    };
    if (!tracking) {
        velocity.y += ballistic_accel * step;
        thrust();
        // TurnRate degrees a tick here, where UpdateTracking turns a tenth
        // of that: a shot with none keeps the facing it left with.
        if (velocity_align) facing = turned_toward(facing, velocity, turn_rate * kDegToRad);
    } else {
        if (!steer(facing, registry, L, terrain, tick)) return; // its script destroyed it
        thrust();
    }
    if (max_speed != 0.0f) {
        const f32 speed =
            std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
        if (speed > max_speed) {
            const f32 cut = max_speed / speed;
            velocity = {velocity.x * cut, velocity.y * cut, velocity.z * cut};
        }
    }
    if (stay_upright) facing = coords_orient(forward_of(facing));

    auto pos = from;
    pos.x += (velocity.x + start_velocity.x) * step * 0.5f;
    pos.y += (velocity.y + start_velocity.y) * step * 0.5f;
    pos.z += (velocity.z + start_velocity.z) * step * 0.5f;

    // SetLocalAngularVelocity: it spins in its own frame, about its forward
    // axis if it aligns or tracks, its up axis if it stays upright, with the
    // rate it was given.
    const f32 spin_sq = angular_velocity.x * angular_velocity.x +
                        angular_velocity.y * angular_velocity.y +
                        angular_velocity.z * angular_velocity.z;
    if (spin_sq > 0.0f) {
        if (velocity_align || tracking || stay_upright) {
            const f32 rate = std::sqrt(spin_sq);
            angular_velocity =
                velocity_align || tracking ? Vector3{0.0f, 0.0f, rate} : Vector3{0.0f, rate, 0.0f};
        }
        const Vector3 turn{angular_velocity.x * step, angular_velocity.y * step,
                           angular_velocity.z * step};
        const f32 angle = std::sqrt(turn.x * turn.x + turn.y * turn.y + turn.z * turn.z);
        const f32 s = osc::dmath::sin(angle * 0.5f) / angle;
        facing = quat_multiply(
            facing, Quaternion{turn.x * s, turn.y * s, turn.z * s, osc::dmath::cos(angle * 0.5f)});
    }

    // A torpedo in the water stays just under its surface; one above it
    // (launched from a surfaced sub) is free to fall in (Moho clamps only
    // below the water, to a hundredth under).
    if (stay_underwater && in_water && terrain && pos.y > terrain->water_elevation() - 0.01f)
        pos.y = terrain->water_elevation() - 0.01f;
    set_position(pos);
    set_orientation(facing);

    // SetScaleVelocity: an effect that grows or shrinks as it flies.
    if (scale_velocity.x != 0 || scale_velocity.y != 0 || scale_velocity.z != 0) {
        const auto grown = [dt](f32 s, f32 v) {
            return std::max(0.0f, s + v * static_cast<f32>(dt));
        };
        set_scale(grown(scale_x(), scale_velocity.x), grown(scale_y(), scale_velocity.y),
                  grown(scale_z(), scale_velocity.z));
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

bool Projectile::steer(Quaternion& facing, EntityRegistry& registry, lua_State* L,
                       const map::Terrain* terrain, u32 tick) {
    // Its aim: its target's aim spot (a unit's target point, anything
    // else's middle) while it has one, a ground target always. Once its
    // target is gone it has none: the script decides (Projectile.lua's
    // OnLostTarget shortens a homing shot's life) and it stops tracking,
    // steering this once more at the last aim if it was sent at something on
    // the ground or the water.
    bool has_target = has_target_position;
    if (target_entity_id > 0) {
        auto* target = registry.find(target_entity_id);
        if (target && !target->destroyed()) {
            target_position = target->is_unit()
                                  ? static_cast<const Unit*>(target)->target_point(target_point)
                                  : collision_centre(*target);
            has_target_position = true;
            has_target = true;
        } else {
            target_entity_id = 0;
            has_target_position = false;
            has_target = false;
        }
    }
    if (!has_target) {
        if (tracking) {
            if (!call_script(L, registry, "OnLostTarget")) return false;
            tracking = false;
        }
        if (!keep_last_aim) return true;
    }
    const Vector3 pos = position();
    Vector3 aim = target_position;

    // LeadTarget: where its target will be by the time it gets there at its
    // top speed, the time found twice over, both times from the aim
    if (lead_target && max_speed > 0.0f && target_entity_id > 0) {
        if (const Entity* target = registry.find(target_entity_id)) {
            const Vector3 v = target->is_unit() ? static_cast<const Unit*>(target)->velocity()
                              : target->is_projectile()
                                  ? static_cast<const Projectile*>(target)->velocity
                                  : Vector3{};
            const Vector3 aim0 = aim;
            const auto lead_from = [&](const Vector3& at) {
                const f32 dx = pos.x - at.x, dy = pos.y - at.y, dz = pos.z - at.z;
                const f32 t = std::sqrt(dx * dx + dy * dy + dz * dz) / max_speed;
                return Vector3{aim0.x + v.x * t, aim0.y + v.y * t, aim0.z + v.z * t};
            };
            aim = lead_from(lead_from(aim0));
        }
    }

    // A torpedo aims at most a quarter under the surface, so one launched
    // above the water dives into it (Moho's underwater aim clamp, M206o).
    const f32 water = water_line(terrain);
    if (stay_underwater) aim.y = std::min(aim.y, water - 0.25f);

    Vector3 toward{aim.x - pos.x, aim.y - pos.y, aim.z - pos.z};

    // Zig-zag: a random offset, redrawn every ZigZagFrequency (in whole
    // ticks), weighted down within MaxZigZag of the aim, on a point its top
    // speed ahead; never under the ground, nor (unless it keeps under) the
    // water.
    if (max_zig_zag > 0.0f && zig_zag_freq > 0.0f) {
        if (zig_zag_next_tick <= tick) {
            SimRandom& rng = registry.sim_random();
            zig_zag_offset.x = rng.range(-max_zig_zag, max_zig_zag);
            zig_zag_offset.y = rng.range(-max_zig_zag, max_zig_zag);
            zig_zag_offset.z = rng.range(-max_zig_zag, max_zig_zag);
            zig_zag_next_tick = tick + static_cast<u32>(static_cast<i32>(zig_zag_freq * 10.0f));
        }
        const f32 dist = std::sqrt(toward.x * toward.x + toward.y * toward.y + toward.z * toward.z);
        const f32 blend = std::min(dist / max_zig_zag, 1.0f);
        const Vector3 dir =
            dist > 0.0f ? Vector3{toward.x / dist, toward.y / dist, toward.z / dist} : Vector3{};
        const f32 jx = pos.x + dir.x * max_speed + zig_zag_offset.x * blend;
        const f32 jz = pos.z + dir.z * max_speed + zig_zag_offset.z * blend;
        const f32 base_y = pos.y + dir.y * max_speed;
        f32 jy = base_y + zig_zag_offset.y * blend;
        const f32 ground = (terrain ? terrain->get_terrain_height(jx, jz) : 0.0f) + 0.5f;
        jy = std::max(jy, std::max(base_y, ground));
        if (!stay_underwater) jy = std::max(jy, std::min(base_y, water + 0.5f));
        toward = {jx - pos.x, jy - pos.y, jz - pos.z};
    }

    // It turns at most TurnRate degrees a second
    facing = turned_toward(facing, toward, turn_rate * kDegToRad * 0.1f);

    // Under the water, one rising toward the surface levels off at it
    if (stay_underwater && in_water) {
        const Vector3 ahead = forward_of(facing);
        if (ahead.y > 0.0f) {
            const f32 t = (water - pos.y) / ahead.y;
            if (t < 1.0f) {
                Vector3 clipped{ahead.x, ahead.y * t, ahead.z};
                const f32 len_sq =
                    clipped.x * clipped.x + clipped.y * clipped.y + clipped.z * clipped.z;
                if (len_sq < 1.0e-6f && len_sq > 0.0f) {
                    const f32 len = std::sqrt(len_sq);
                    clipped = {clipped.x / len, clipped.y / len, clipped.z / len};
                }
                facing = coords_orient(clipped);
            }
        }
    }

    // VelocityAlign: what of its velocity lies along its new facing
    if (velocity_align) {
        const Vector3 ahead = forward_of(facing);
        const f32 len_sq = ahead.x * ahead.x + ahead.y * ahead.y + ahead.z * ahead.z;
        const f32 along =
            len_sq > 0.0f
                ? (velocity.x * ahead.x + velocity.y * ahead.y + velocity.z * ahead.z) / len_sq
                : 0.0f;
        velocity = {ahead.x * along, ahead.y * along, ahead.z * along};
    }
    return true;
}

void Projectile::arm_lost_target_aim(const EntityRegistry& registry) {
    // Moho's Projectile constructor: a homing shot sent at something neither
    // in the air nor under the water steers on at where it last was.
    keep_last_aim = false;
    if (!tracking || target_entity_id == 0) return;
    const Entity* target = registry.find(target_entity_id);
    if (!target || !target->is_unit()) return;
    const std::string layer = static_cast<const Unit*>(target)->layer();
    keep_last_aim = layer != "Air" && layer != "Sub";
}

namespace {

Vector3 lerp(const Vector3& a, const Vector3& b, f32 t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
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
    // What is struck is asked, OnCollisionCheck(self, other) with `other`
    // the projectile; the projectile itself is not (faf-re Projectile::
    // CheckCollision calls only the collided entity's script). A missing one
    // allows it. Retail's comment: "If we return false the thing hitting us
    // has no idea that it came into contact with us". FAF's torpedo answers
    // false to anything but an anti-torpedo projectile hitting it, and had
    // been asked about every ship it ran into.
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
    return check(other_id, self_id);
}

Projectile::BlueprintPhysics Projectile::apply_blueprint_physics(lua_State* L, SimRandom* rng) {
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
    flag("LeadTarget", lead_target);
    number("MaxZigZag", max_zig_zag);
    number("ZigZagFrequency", zig_zag_freq);
    flag("StayUnderwater", stay_underwater);
    // Where it ends of itself: on the water (209 retail shells), or
    // bursting at a height above the surface.
    flag("DestroyOnWater", destroy_on_water);
    number("DetonateAboveHeight", detonate_above_height);
    number("DetonateBelowHeight", detonate_below_height);
    // Strategic missiles rise from their silos through the ground.
    flag("CollideSurface", collide_surface);
    // And some hit no entity (Moho's mDoCollision, from Physics.CollideEntity:
    // FAF's UEF build beams end on dummy projectiles made inside the unit
    // being built, which would otherwise hit it and be gone at once).
    flag("CollideEntity", collide_entity);
    if (field("UseGravity") == LUA_TBOOLEAN) found.use_gravity = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    found.realistic_ordinance =
        field("RealisticOrdinance") != LUA_TNIL && lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    if (field("Lifetime") == LUA_TNUMBER) found.lifetime = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    // Each made a little different: base +- its range, uniformly (Moho's
    // RandomSymmetricAround). Moho draws even for a range of 0; this only
    // for one there is.
    if (rng) {
        f32 spin = 0;
        f32 spin_range = 0;
        number("RotationalVelocity", spin);
        number("RotationalVelocityRange", spin_range);
        if (spin != 0.0f || spin_range != 0.0f) {
            Vector3 axis{gaussian(*rng), gaussian(*rng), gaussian(*rng)};
            const f32 length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
            const f32 rate = (spin + rng->range(-spin_range, spin_range)) * kDegToRad / length;
            angular_velocity = {axis.x * rate, axis.y * rate, axis.z * rate};
        }
        const auto spread = [&](const char* key, f32& value) {
            f32 range = 0;
            number(key, range);
            if (range != 0.0f) value += rng->range(-range, range);
        };
        spread("TurnRateRange", turn_rate);
        spread("MaxSpeedRange", max_speed);
        spread("AccelerationRange", acceleration);
        if (found.lifetime) spread("LifetimeRange", *found.lifetime);
    }
    lua_settop(L, top);
    return found;
}

const std::unordered_set<std::string>& Projectile::categories() const {
    // One with no blueprint (or none read yet) is just a projectile.
    static const std::unordered_set<std::string> kBare{"ALLPROJECTILES"};
    return info_ ? info_->categories : kBare;
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

bool Projectile::has_live_target(const EntityRegistry& registry) const {
    if (target_entity_id == 0) return has_target_position;
    const Entity* target = registry.find(target_entity_id);
    if (!target || target->destroyed()) return false;
    return !target->is_unit() || !static_cast<const Unit*>(target)->is_dying();
}

} // namespace osc::sim
