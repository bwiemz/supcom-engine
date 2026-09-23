#include "sim/weapon.hpp"
#include "sim/projectile_script.hpp"
#include "core/dmath.hpp"
#include "core/test_status.hpp"
#include "sim/bone_data.hpp"
#include "sim/entity_registry.hpp"
#include "sim/projectile.hpp"
#include "sim/unit.hpp"
#include "map/visibility_grid.hpp"

#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::sim {

namespace {

bool has_omni_detection(const Unit& owner, const Entity& target,
                        const map::VisibilityGrid* visibility_grid) {
    if (!visibility_grid || owner.army() < 0) return false;
    const auto& pos = target.position();
    return visibility_grid->has_omni(pos.x, pos.z,
                                     static_cast<u32>(owner.army()));
}

bool is_weapon_targetable(const Unit& owner, const Entity& target,
                          const map::VisibilityGrid* visibility_grid) {
    if (!target.is_unit()) return true;
    const auto* target_unit = static_cast<const Unit*>(&target);
    if (target_unit->is_cloaked() &&
        !has_omni_detection(owner, target, visibility_grid)) {
        return false;
    }
    return true;
}

} // namespace

u32 Weapon::fire_period() const {
    if (rate_of_fire <= 0) return 10;
    const f64 ticks = std::floor(10.0 / static_cast<f64>(rate_of_fire) + 0.5);
    return static_cast<u32>(std::clamp(ticks, 1.0, 1.0e6));
}

bool Weapon::can_fire(const Unit& owner, const EntityRegistry& registry) const {
    if (!enabled || target_entity_id == 0 || owner.busy()) return false;
    // A script callback earlier this tick may have destroyed the target.
    const Entity* target = registry.find(target_entity_id);
    if (!target || target->destroyed()) return false;
    if (need_compute_bomb_drop) {
        const f32 dx = target->position().x - owner.position().x;
        const f32 dz = target->position().z - owner.position().z;
        if (dx * dx + dz * dz > bomb_drop_threshold * bomb_drop_threshold) return false;
    }
    return true;
}

bool Weapon::call_script(lua_State* L, const char* method) const {
    if (!L || lua_table_ref < 0) return true;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref);
    const int self = lua_gettop(L);
    lua_pushstring(L, method);
    lua_gettable(L, self);
    bool result = true;
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, self);
        if (lua_pcall(L, 1, 1, 0) != 0) {
            const char* err = lua_tostring(L, -1);
            const std::string message =
                "Weapon " + label + " " + method + " error: " + (err ? err : "(unknown)");
            spdlog::warn("{}", message);
            if (test_status::count_lua_failures()) test_status::record_failure(message);
            result = false;
        } else {
            result = lua_toboolean(L, -1) != 0;
        }
    }
    lua_settop(L, top);
    return result;
}

void Weapon::update(Unit& owner, EntityRegistry& registry, lua_State* L,
                    const map::VisibilityGrid* visibility_grid) {
    if (fire_clock > 0) --fire_clock;

    if (!enabled || fire_on_death || manual_fire) return;
    if (max_range <= 0 || damage <= 0) return;
    // HoldFire (1) = don't auto-target or fire at all
    if (owner.fire_state() == 1) return;

    const u32 previous_target = target_entity_id;
    update_targeting(owner, registry, visibility_grid);

    if (L && fires_through_script()) {
        update_scripted(owner, registry, L, previous_target);
        return;
    }

    if (target_entity_id == 0 || fire_clock > 0) return;
    if (try_fire(owner, registry, L, visibility_grid)) fire_clock = fire_period();
}

void Weapon::update_scripted(Unit& owner, EntityRegistry& registry, lua_State* L,
                             u32 previous_target) {
    // Each callback may kill the unit or disable the weapon.
    const auto still_firing = [&] {
        return !owner.destroyed() && !owner.is_dying() && fires_through_script();
    };
    if (target_entity_id != previous_target) {
        if (previous_target != 0) {
            call_script(L, "OnLostTarget");
            if (!still_firing()) return;
        }
        if (target_entity_id != 0) {
            call_script(L, "OnGotTarget");
            if (!still_firing()) return;
        }
    }

    // The fire clock: when it is ready and the weapon can fire, the script
    // gets OnFire (its state machine decides what that means) and the clock
    // restarts.
    if (fire_clock > 0 || !can_fire(owner, registry)) return;
    if (!call_script(L, "CanWeaponFire") || !still_firing()) return;
    call_script(L, "OnFire");
    fire_clock = fire_period();
}

void Weapon::update_targeting(Unit& owner, EntityRegistry& registry,
                              const map::VisibilityGrid* visibility_grid) {
    // Check if current target is still valid
    if (target_entity_id > 0) {
        auto* target = registry.find(target_entity_id);
        if (target && !target->destroyed() && target->is_unit() &&
            !target->do_not_target() &&
            is_weapon_targetable(owner, *target, visibility_grid)) {
            // Layer cap check on existing target
            if (fire_target_layer_caps != 0xFF &&
                !(layer_to_bit(static_cast<Unit*>(target)->layer()) & fire_target_layer_caps)) {
                target_entity_id = 0;
            } else {
                // Check range (3D distance)
                f32 dx = target->position().x - owner.position().x;
                f32 dy = target->position().y - owner.position().y;
                f32 dz = target->position().z - owner.position().z;
                f32 dist2 = dx * dx + dy * dy + dz * dz;
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
        if (!is_weapon_targetable(owner, *e, visibility_grid)) continue;
        // Layer cap filter
        if (fire_target_layer_caps != 0xFF &&
            !(layer_to_bit(static_cast<Unit*>(e)->layer()) & fire_target_layer_caps))
            continue;

        f32 dx = e->position().x - owner.position().x;
        f32 dy = e->position().y - owner.position().y;
        f32 dz = e->position().z - owner.position().z;
        f32 dist2 = dx * dx + dy * dy + dz * dz;
        if (dist2 < min2) continue;
        if (dist2 < best_dist2) {
            best_dist2 = dist2;
            best_id = id;
        }
    }

    target_entity_id = best_id;
}

bool Weapon::try_fire(Unit& owner, EntityRegistry& registry,
                      lua_State* L,
                      const map::VisibilityGrid* visibility_grid) {
    auto* target = registry.find(target_entity_id);
    if (!target || target->destroyed() || target->do_not_target() ||
        !is_weapon_targetable(owner, *target, visibility_grid) ||
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

    const std::string& layer = owner.layer();
    const Projectile* fired =
        launch(owner, spawn_pos, target, registry, L, layer == "Sub" || layer == "Seabed");
    if (!fired) return false;
    spdlog::debug("Weapon '{}' fired projectile #{} at entity #{}", label, fired->entity_id(),
                  target_entity_id);
    return true;
}

Projectile* Weapon::launch(Unit& owner, const Vector3& spawn_pos, const Entity* target,
                           EntityRegistry& registry, lua_State* L, bool in_water) {
    // Toward the target, or along the owner's facing with none.
    f32 dx, dz, dist;
    if (target) {
        dx = target->position().x - spawn_pos.x;
        dz = target->position().z - spawn_pos.z;
        dist = std::sqrt(dx * dx + dz * dz);
    } else {
        const Vector3 forward = quat_rotate(owner.orientation(), Vector3{0.0f, 0.0f, 1.0f});
        dx = forward.x;
        dz = forward.z;
        dist = std::sqrt(dx * dx + dz * dz);
        const f32 reach = max_range > 0 ? max_range : 10.0f;
        if (dist > 0.001f) {
            dx *= reach / dist;
            dz *= reach / dist;
            dist = reach;
        }
    }
    if (dist < 0.001f) dist = 0.001f;

    f32 inv_dist = 1.0f / dist;
    Vector3 vel;
    vel.x = dx * inv_dist * muzzle_velocity;
    vel.y = 0;
    vel.z = dz * inv_dist * muzzle_velocity;

    // Apply firing randomness as angular offset to velocity direction.
    // Drawn from the deterministic sim RNG so every lockstep client rolls the
    // same spread (a per-process std::random_device would desync clients).
    if (firing_randomness > 0) {
        f32 angle = registry.sim_random().range(-firing_randomness, firing_randomness);
        f32 c = osc::dmath::cos(angle), s = osc::dmath::sin(angle);
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
    proj->target_entity_id = (need_compute_bomb_drop || !target) ? 0 : target->entity_id();
    proj->target_position =
        target ? target->position() : Vector3{spawn_pos.x + dx, spawn_pos.y, spawn_pos.z + dz};
    proj->launcher_id = owner.entity_id();
    proj->damage_amount = damage * owner.damage_multiplier();
    proj->damage_radius = damage_radius;
    proj->damage_type = damage_type;
    // Bombs drop from altitude so need more time; normal projectiles use flight time
    proj->lifetime = (need_compute_bomb_drop || muzzle_velocity <= 0)
                         ? 10.0f // generous for high-altitude drops
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
    f32 heading = osc::dmath::atan2(vel.x, vel.z);
    proj->set_orientation(euler_to_quat(heading, 0.0f, 0.0f));

    u32 proj_id = registry.register_entity(std::move(proj));
    auto* proj_ptr = static_cast<Projectile*>(registry.find(proj_id));
    if (proj_ptr) create_projectile_object(L, *proj_ptr, in_water, false);
    return proj_ptr;
}

} // namespace osc::sim
