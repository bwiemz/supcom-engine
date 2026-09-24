#include "sim/weapon.hpp"
#include "sim/projectile_script.hpp"
#include "core/dmath.hpp"
#include "core/test_status.hpp"
#include "sim/bone_data.hpp"
#include "sim/entity_registry.hpp"
#include "sim/projectile.hpp"
#include "sim/sim_state.hpp"
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

bool is_underwater(const std::string& layer) {
    return layer == "Sub" || layer == "Seabed";
}

/// The unit an attack order at the head of the queue names, or 0.
u32 attack_order_target(const Unit& owner) {
    const auto& queue = owner.command_queue();
    if (queue.empty() || queue.front().type != CommandType::Attack) return 0;
    return queue.front().target_id;
}

} // namespace

u32 Weapon::fire_period() const {
    if (rate_of_fire <= 0) return 10;
    const f64 ticks = std::floor(10.0 / static_cast<f64>(rate_of_fire) + 0.5);
    return static_cast<u32>(std::clamp(ticks, 1.0, 1.0e6));
}

bool Weapon::can_fire(const Unit& owner, const EntityRegistry& registry) const {
    if (!enabled || target_entity_id == 0 || owner.busy()) return false;
    if (above_water_fire_only && is_underwater(owner.layer())) return false;
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
                    const map::VisibilityGrid* visibility_grid, const SimState* sim) {
    if (fire_clock > 0) --fire_clock;

    if (!enabled || fire_on_death || manual_fire) return;
    if (max_range <= 0 || damage <= 0) return;
    // HoldFire (1) = don't auto-target or fire at all
    if (owner.fire_state() == 1) return;

    const u32 previous_target = target_entity_id;
    update_targeting(owner, registry, visibility_grid, sim);

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

bool Weapon::can_target(const Unit& owner, const Entity& target,
                        const map::VisibilityGrid* visibility_grid, const SimState* sim) const {
    if (target.destroyed() || !target.is_unit() || target.entity_id() == owner.entity_id())
        return false;
    if (target.do_not_target() || target.army() < 0) return false;
    if (sim ? !sim->is_enemy(owner.army(), target.army()) : target.army() == owner.army())
        return false;
    if (!is_weapon_targetable(owner, target, visibility_grid)) return false;
    const auto& unit = static_cast<const Unit&>(target);
    if (fire_target_layer_caps != 0xFF && !(layer_to_bit(unit.layer()) & fire_target_layer_caps))
        return false;
    if (above_water_targets_only && is_underwater(unit.layer())) return false;
    if (!restrict_only_allow.empty() && !restrict_only_allow.matches(unit.categories()))
        return false;
    if (restrict_disallow.matches(unit.categories())) return false;

    const f32 dx = target.position().x - owner.position().x;
    const f32 dz = target.position().z - owner.position().z;
    const f32 dist2 = dx * dx + dz * dz;
    if (dist2 > max_range * max_range || dist2 < min_range * min_range) return false;
    if (max_height_diff > 0 &&
        std::fabs(target.position().y - owner.position().y) > max_height_diff)
        return false;
    return true;
}

int Weapon::priority_of(const Unit& target) const {
    if (target_priorities.empty()) return 0;
    for (size_t i = 0; i < target_priorities.size(); ++i) {
        if (target_priorities[i].matches(target.categories())) return static_cast<int>(i);
    }
    return -1;
}

void Weapon::update_targeting(Unit& owner, EntityRegistry& registry,
                              const map::VisibilityGrid* visibility_grid, const SimState* sim) {
    // A target this weapon can no longer shoot is dropped at once.
    if (target_entity_id != 0) {
        const Entity* target = registry.find(target_entity_id);
        if (!target || !can_target(owner, *target, visibility_grid, sim)) target_entity_id = 0;
    }

    // An attack order's target comes first, for every weapon that can hit
    // it, whatever the priorities say.
    if (const u32 ordered = attack_order_target(owner); ordered != 0) {
        if (ordered == target_entity_id) return;
        const Entity* target = registry.find(ordered);
        if (target && can_target(owner, *target, visibility_grid, sim)) {
            target_entity_id = ordered;
            return;
        }
    }

    // Otherwise look for targets every TargetCheckInterval: when there is
    // none, or, with AlwaysRecheckTarget, for one of a better priority.
    if (target_check_clock > 0) {
        --target_check_clock;
        return;
    }
    target_check_clock = target_check_period > 0 ? target_check_period - 1 : 0;
    int current_priority = -1;
    if (target_entity_id != 0) {
        if (!always_recheck_target) return;
        current_priority = priority_of(static_cast<const Unit&>(*registry.find(target_entity_id)));
    }

    // Best: the earliest priority, then the nearest, then the lowest id
    // (candidates come in id order, so ties keep the first).
    u32 best_id = 0;
    int best_priority = 0;
    f32 best_dist2 = 0;
    for (const u32 id :
         registry.collect_in_radius(owner.position().x, owner.position().z, max_range)) {
        const Entity* e = registry.find(id);
        if (!e || !can_target(owner, *e, visibility_grid, sim)) continue;
        const int priority = priority_of(static_cast<const Unit&>(*e));
        if (priority < 0) continue;
        const f32 dx = e->position().x - owner.position().x;
        const f32 dz = e->position().z - owner.position().z;
        const f32 dist2 = dx * dx + dz * dz;
        if (best_id == 0 || priority < best_priority ||
            (priority == best_priority && dist2 < best_dist2)) {
            best_id = id;
            best_priority = priority;
            best_dist2 = dist2;
        }
    }
    // A recheck changes target only for a better priority.
    if (current_priority >= 0 && (best_id == 0 || best_priority >= current_priority)) return;
    if (best_id != 0) target_entity_id = best_id;
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
