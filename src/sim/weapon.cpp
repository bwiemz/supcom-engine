#include "sim/weapon.hpp"
#include "sim/projectile_script.hpp"
#include "core/dmath.hpp"
#include "core/test_status.hpp"
#include "sim/bone_data.hpp"
#include "sim/collision.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
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

constexpr f32 kGravity = Projectile::GRAVITY;
constexpr f32 kPi = 3.14159265358979f;

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

/// The categories a target answers to: a unit's or a projectile's.
const std::unordered_set<std::string>& target_categories(const Entity& target) {
    static const std::unordered_set<std::string> kNone;
    if (target.is_unit()) return static_cast<const Unit&>(target).categories();
    if (target.is_projectile()) return static_cast<const Projectile&>(target).categories();
    return kNone;
}

/// Whether `proj` has room for another shooter under its DesiredShooterCap.
/// Its list of shooters is pruned first of weapons that have let it go.
bool shooter_room(Projectile& proj, const EntityRegistry& registry) {
    const u32 cap = proj.desired_shooter_cap();
    if (cap == 0) return true;
    auto& shooters = proj.shooters;
    shooters.erase(std::remove_if(shooters.begin(), shooters.end(),
                                  [&](const std::pair<u32, i32>& s) {
                                      const Entity* e = registry.find(s.first);
                                      if (!e || e->destroyed() || !e->is_unit()) return true;
                                      const auto& weapons = static_cast<const Unit*>(e)->weapons();
                                      return s.second < 0 ||
                                             static_cast<size_t>(s.second) >= weapons.size() ||
                                             weapons[static_cast<size_t>(s.second)]
                                                     ->target_entity_id != proj.entity_id();
                                  }),
                   shooters.end());
    return shooters.size() < cap;
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

std::optional<Vector3> Weapon::target_point(const EntityRegistry& registry) const {
    if (target_entity_id != 0) {
        const Entity* target = registry.find(target_entity_id);
        if (!target || target->destroyed()) return std::nullopt;
        return target->position();
    }
    if (has_ground_target) return ground_target;
    return std::nullopt;
}

bool Weapon::can_fire(const Unit& owner, const EntityRegistry& registry) const {
    if (!enabled || !has_target() || owner.busy()) return false;
    if (counted_projectile && owner.silo_ammo(nuke_weapon) <= 0) return false;
    if (above_water_fire_only && is_underwater(owner.layer())) return false;
    // A script callback earlier this tick may have destroyed the target.
    const std::optional<Vector3> at = target_point(registry);
    if (!at) return false;
    // Tracked from farther (TrackingRadius), fired at only within MaxRadius,
    // and only once the fire control is on target.
    if (!in_firing_range(owner, *at)) return false;
    if (const AimManipulator* aim = fire_control(owner);
        aim && !(aim->enabled() && aim->on_target()))
        return false;
    if (need_compute_bomb_drop) {
        const f32 dx = at->x - owner.position().x;
        const f32 dz = at->z - owner.position().z;
        if (dx * dx + dz * dz > bomb_drop_threshold * bomb_drop_threshold) return false;
    }
    return true;
}

bool Weapon::call_script(lua_State* L, const char* method, const char* arg) const {
    if (!L || lua_table_ref < 0) return true;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref);
    const int self = lua_gettop(L);
    lua_pushstring(L, method);
    lua_gettable(L, self);
    bool result = true;
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, self);
        if (arg) lua_pushstring(L, arg);
        if (lua_pcall(L, arg ? 2 : 1, 1, 0) != 0) {
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

    if (!enabled || fire_on_death) return;
    // A nuke's damage is its script's (Damage 0): a manual weapon fires
    // whatever its damage.
    if (max_range <= 0 || (damage <= 0 && !manual_fire)) return;

    const TargetMark previous = target_mark();
    if (manual_fire) {
        // It fires only at what its unit's launch order names, whatever the
        // fire state.
        take_order_target(owner, registry);
    } else {
        // HoldFire (1) = don't auto-target or fire at all
        if (owner.fire_state() == 1) return;
        update_targeting(owner, registry, visibility_grid, sim);
    }
    update_aim(owner, registry, L);
    if (owner.destroyed() || owner.is_dying()) return; // a tracking callback may kill it

    if (L && fires_through_script()) {
        update_scripted(owner, registry, L, previous);
        return;
    }

    if (!has_target() || fire_clock > 0) return;
    const std::optional<Vector3> at = target_point(registry);
    if (!at || !in_firing_range(owner, *at)) return;
    if (counted_projectile && owner.silo_ammo(nuke_weapon) <= 0) return;
    if (const AimManipulator* aim = fire_control(owner);
        aim && !(aim->enabled() && aim->on_target()))
        return;
    if (try_fire(owner, registry, L, visibility_grid)) fire_clock = fire_period();
}

void Weapon::take_order_target(const Unit& owner, const EntityRegistry& registry) {
    const UnitCommand* order = owner.launch_order_for(*this);
    if (!order) {
        set_target_entity(0);
        return;
    }
    if (order->target_id == 0) {
        set_target_ground(order->target_pos);
    } else {
        // A unit it was sent at that is gone: nothing (the order ends).
        const Entity* target = registry.find(order->target_id);
        set_target_entity(target && !target->destroyed() ? order->target_id : 0);
    }
    if (const std::optional<Vector3> at = target_point(registry)) last_order_point = at;
}

void Weapon::update_scripted(Unit& owner, EntityRegistry& registry, lua_State* L,
                             const TargetMark& previous) {
    // Each callback may kill the unit or disable the weapon.
    const auto still_firing = [&] {
        return !owner.destroyed() && !owner.is_dying() && fires_through_script();
    };
    if (!(target_mark() == previous)) {
        if (previous.entity != 0 || previous.ground) {
            // A manual weapon whose order went: its script must not fire on
            // (Moho's OnHaltFire, "so we won't fire if the target reticle is
            // moved"); an unpacking launcher packs up on losing its target.
            if (manual_fire) {
                call_script(L, "OnHaltFire");
                if (!still_firing()) return;
            }
            call_script(L, "OnLostTarget");
            if (!still_firing()) return;
        }
        if (has_target()) {
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
    // A weapon shoots units, or, with TargetType RULEWTT_Projectile, the
    // other side's projectiles (M206b).
    if (target.destroyed() || target.entity_id() == owner.entity_id()) return false;
    if (targets_projectiles ? !target.is_projectile() : !target.is_unit()) return false;
    if (target.do_not_target() || target.army() < 0) return false;
    if (sim ? !sim->is_enemy(owner.army(), target.army()) : target.army() == owner.army())
        return false;
    if (target.is_unit()) {
        if (!is_weapon_targetable(owner, target, visibility_grid)) return false;
        const auto& unit = static_cast<const Unit&>(target);
        if (fire_target_layer_caps != 0xFF &&
            !(layer_to_bit(unit.layer()) & fire_target_layer_caps))
            return false;
        if (above_water_targets_only && is_underwater(unit.layer())) return false;
    } else {
        // A projectile in flight is on the Air layer, or Water below the
        // surface.
        const auto& proj = static_cast<const Projectile&>(target);
        if (proj.impacted) return false;
        if (fire_target_layer_caps != 0xFF &&
            !(layer_to_bit(proj.layer()) & fire_target_layer_caps))
            return false;
        if (above_water_targets_only && proj.in_water) return false;
    }
    const auto& categories = target_categories(target);
    if (!restrict_only_allow.empty() && !restrict_only_allow.matches(categories)) return false;
    if (restrict_disallow.matches(categories)) return false;

    const f32 dx = target.position().x - owner.position().x;
    const f32 dz = target.position().z - owner.position().z;
    const f32 dist2 = dx * dx + dz * dz;
    const f32 reach = max_range * std::max(1.0f, tracking_radius);
    if (dist2 > reach * reach || dist2 < min_range * min_range) return false;
    if (heading_arc_range < 180.0f) {
        // Only targets within the arc about the unit's facing.
        const Vector3 forward = quat_rotate(owner.orientation(), Vector3{0.0f, 0.0f, 1.0f});
        constexpr f32 kDegToRad = 3.14159265358979f / 180.0f;
        f32 off = osc::dmath::atan2(dx, dz) - osc::dmath::atan2(forward.x, forward.z) -
                  heading_arc_center * kDegToRad;
        while (off > 3.14159265f) off -= 6.28318531f;
        while (off < -3.14159265f) off += 6.28318531f;
        if (std::fabs(off) > heading_arc_range * kDegToRad) return false;
    }
    if (max_height_diff > 0 &&
        std::fabs(target.position().y - owner.position().y) > max_height_diff)
        return false;
    return true;
}

int Weapon::priority_of(const Entity& target) const {
    if (target_priorities.empty()) return 0;
    const auto& categories = target_categories(target);
    for (size_t i = 0; i < target_priorities.size(); ++i) {
        if (target_priorities[i].matches(categories)) return static_cast<int>(i);
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
            set_target_entity(ordered);
            return;
        }
    }

    // A ground target its script set stays until the script changes it.
    if (has_ground_target) return;

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
        current_priority = priority_of(*registry.find(target_entity_id));
    }

    // Best: the earliest priority, then the nearest, then the lowest id
    // (candidates come in id order, so ties keep the first).
    u32 best_id = 0;
    int best_priority = 0;
    f32 best_dist2 = 0;
    const f32 reach = max_range * std::max(1.0f, tracking_radius);
    for (const u32 id : registry.collect_in_radius(owner.position().x, owner.position().z, reach)) {
        Entity* e = registry.find(id);
        if (!e || !can_target(owner, *e, visibility_grid, sim)) continue;
        // A missile already held by as many weapons as it wants shooting at
        // it (a nuke's DesiredShooterCap is 1) is left to them.
        if (e->is_projectile() && !shooter_room(static_cast<Projectile&>(*e), registry)) continue;
        const int priority = priority_of(*e);
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
    if (best_id == 0) return;
    target_entity_id = best_id;
    if (Entity* target = registry.find(best_id); target && target->is_projectile())
        static_cast<Projectile&>(*target).shooters.emplace_back(owner.entity_id(), weapon_index);
}

bool Weapon::in_firing_range(const Unit& owner, const Entity& target) const {
    return in_firing_range(owner, target.position());
}

bool Weapon::in_firing_range(const Unit& owner, const Vector3& at) const {
    const f32 dx = at.x - owner.position().x;
    const f32 dz = at.z - owner.position().z;
    const f32 dist2 = dx * dx + dz * dz;
    return dist2 <= max_range * max_range && dist2 >= min_range * min_range;
}

const AimManipulator* Weapon::fire_control(const Unit& owner) const {
    const AimManipulator* first = nullptr;
    for (const auto& m : owner.manipulators()) {
        const auto* aim = dynamic_cast<const AimManipulator*>(m.get());
        if (!aim || aim->is_destroyed() || aim->weapon_index() != weapon_index) continue;
        if (!fire_control_label.empty() && aim->label() == fire_control_label) return aim;
        if (!first) first = aim;
    }
    return first;
}

void Weapon::update_aim(Unit& owner, EntityRegistry& registry, lua_State* L) {
    // Collect first: a tracking callback may add manipulators (the vector
    // may grow) or destroy them (they stay allocated until the unit's
    // manipulators tick).
    std::vector<AimManipulator*> aims;
    for (const auto& m : owner.manipulators()) {
        auto* aim = dynamic_cast<AimManipulator*>(m.get());
        if (aim && !aim->is_destroyed() && aim->weapon_index() == weapon_index) aims.push_back(aim);
    }
    if (aims.empty()) return;
    const Entity* target = target_entity_id != 0 ? registry.find(target_entity_id) : nullptr;
    const bool aiming = target || has_ground_target;
    constexpr f32 kDegToRad = 3.14159265358979f / 180.0f;
    const bool scripted = script_class && L;
    for (AimManipulator* aim : aims) {
        if (aim->is_destroyed()) continue;
        const bool was_tracking = aim->has_target();
        if (aiming) {
            const Vector3 from = owner.position();
            const Vector3 at = target ? aim_point(*target, from) : ground_target;
            aim->set_target(at, firing_tolerance * kDegToRad);
            if (ballistic_arc != Arc::None && !need_compute_bomb_drop) {
                const f32 dx = at.x - from.x;
                const f32 dz = at.z - from.z;
                aim->set_elevation(launch_elevation(std::sqrt(dx * dx + dz * dz), at.y - from.y));
            } else {
                aim->set_elevation(std::nullopt);
            }
        } else {
            aim->clear_target();
        }
        if (!scripted || was_tracking == aiming) continue;
        const std::string label = aim->label(); // the callback may free the aim
        call_script(L, aiming ? "OnStartTracking" : "OnStopTracking", label.c_str());
        if (owner.destroyed() || owner.is_dying()) return;
    }
}

bool Weapon::try_fire(Unit& owner, EntityRegistry& registry,
                      lua_State* L,
                      const map::VisibilityGrid* visibility_grid) {
    // At a unit, or at its ground target.
    auto* target = target_entity_id != 0 ? registry.find(target_entity_id) : nullptr;
    if (target_entity_id != 0 &&
        (!target || target->destroyed() || target->do_not_target() ||
         !is_weapon_targetable(owner, *target, visibility_grid) ||
         (fire_target_layer_caps != 0xFF && target->is_unit() &&
          !(layer_to_bit(static_cast<Unit*>(target)->layer()) & fire_target_layer_caps)))) {
        target_entity_id = 0;
        return false;
    }
    if (!target && !has_ground_target) return false;
    const Vector3 at = target ? target->position() : ground_target;

    // Bomb drop check: only fire when directly overhead
    if (need_compute_bomb_drop) {
        f32 dx = at.x - owner.position().x;
        f32 dz = at.z - owner.position().z;
        f32 horiz_dist = std::sqrt(dx * dx + dz * dz);
        if (horiz_dist > bomb_drop_threshold)
            return false; // Not overhead yet — don't fire
    }

    // Resolve muzzle bone position for projectile spawn
    // From the muzzle as the turret is posed now.
    Vector3 spawn_pos = owner.position();
    std::optional<Vector3> muzzle_dir;
    if (!muzzle_bone_name.empty() && owner.bone_data()) {
        const i32 bone = owner.bone_data()->find_bone(muzzle_bone_name);
        if (bone >= 0) {
            spawn_pos = owner.bone_world_position(bone);
            muzzle_dir = owner.bone_world_forward(bone);
        }
    }

    const std::string& layer = owner.layer();
    const Projectile* fired = launch(owner, spawn_pos, target, registry, L,
                                     layer == "Sub" || layer == "Seabed", muzzle_dir);
    if (!fired) return false;
    // With no script to take it, the engine spends the missile.
    if (counted_projectile) owner.remove_silo_ammo(nuke_weapon, 1);
    spdlog::debug("Weapon '{}' fired projectile #{} at entity #{}", label, fired->entity_id(),
                  target_entity_id);
    return true;
}

f32 Weapon::launch_elevation(f32 dist, f32 rise) const {
    // tan(theta) = (v^2 -+ sqrt(v^4 - g(g d^2 + 2 h v^2))) / (g d): the low
    // and the high arc through a point `dist` away and `rise` above. Out of
    // reach there is none: 45 degrees goes furthest.
    // Right overhead (or below) the arcs meet the vertical: up for the high
    // arc or a target above, down onto one below for the low arc.
    if (dist <= 0.001f) {
        if (ballistic_arc == Arc::High || rise > 0) return kPi * 0.5f;
        return rise < 0 ? -kPi * 0.5f : 0.0f;
    }
    const f32 v2 = muzzle_velocity * muzzle_velocity;
    const f32 disc = v2 * v2 - kGravity * (kGravity * dist * dist + 2.0f * rise * v2);
    if (muzzle_velocity <= 0 || disc < 0) return kPi * 0.25f;
    const f32 root = std::sqrt(disc);
    return osc::dmath::atan2(ballistic_arc == Arc::High ? v2 + root : v2 - root, kGravity * dist);
}

Vector3 Weapon::aim_point(const Entity& target, const Vector3& from) const {
    // The middle of what it is shooting at, not its feet.
    Vector3 at = collision_centre(target);
    if (!lead_target || muzzle_velocity <= 0) return at;
    // Where it will be when the shot arrives (a unit, or a missile): the
    // flight time to where it is now, once refined.
    Vector3 v;
    if (target.is_unit()) v = static_cast<const Unit&>(target).velocity();
    else if (target.is_projectile()) v = static_cast<const Projectile&>(target).velocity;
    else return at;
    for (int pass = 0; pass < 2; ++pass) {
        const f32 dx = at.x - from.x;
        const f32 dz = at.z - from.z;
        const f32 dist = std::sqrt(dx * dx + dz * dz);
        f32 across = muzzle_velocity;
        if (ballistic_arc != Arc::None)
            across *= osc::dmath::cos(launch_elevation(dist, at.y - from.y));
        const f32 time = across > 0.001f ? dist / across : 0.0f;
        const Vector3 now = collision_centre(target);
        at = {now.x + v.x * time, now.y + v.y * time, now.z + v.z * time};
    }
    return at;
}

Projectile* Weapon::launch(Unit& owner, const Vector3& spawn_pos, const Entity* target,
                           EntityRegistry& registry, lua_State* L, bool in_water,
                           std::optional<Vector3> muzzle_dir) {
    // Where the shot goes: the target (where it will be, for a weapon that
    // leads), its ground target, where a manual weapon's last order sent it,
    // or along the owner's facing to its reach with none.
    Vector3 aim;
    if (target) {
        aim = aim_point(*target, spawn_pos);
    } else if (has_ground_target) {
        aim = ground_target;
    } else if (manual_fire && last_order_point) {
        aim = *last_order_point;
    } else {
        const Vector3 forward = quat_rotate(owner.orientation(), Vector3{0.0f, 0.0f, 1.0f});
        const f32 len = std::sqrt(forward.x * forward.x + forward.z * forward.z);
        const f32 reach = max_range > 0 ? max_range : 10.0f;
        aim = len > 0.001f ? Vector3{spawn_pos.x + forward.x / len * reach, spawn_pos.y,
                                     spawn_pos.z + forward.z / len * reach}
                           : Vector3{spawn_pos.x, spawn_pos.y, spawn_pos.z + reach};
    }
    // Firing randomness scatters where it goes over a circle about the aim,
    // FiringRandomness x distance / 12 across: the relation FAF measured of
    // Moho's (FixedSpreadRadius). Drawn from the sim's RNG, so every
    // lockstep client rolls the same.
    if (firing_randomness > 0) {
        const f32 ox = aim.x - spawn_pos.x;
        const f32 oz = aim.z - spawn_pos.z;
        const f32 radius = firing_randomness * std::sqrt(ox * ox + oz * oz) / 12.0f;
        const f32 angle = registry.sim_random().range(0.0f, 2.0f * kPi);
        const f32 off = radius * std::sqrt(registry.sim_random().range(0.0f, 1.0f));
        aim.x += off * osc::dmath::cos(angle);
        aim.z += off * osc::dmath::sin(angle);
    }
    f32 dx = aim.x - spawn_pos.x;
    f32 dz = aim.z - spawn_pos.z;
    const f32 dy = aim.y - spawn_pos.y;
    f32 dist = std::sqrt(dx * dx + dz * dz);
    if (dist < 0.001f) dist = 0.001f;

    // Straight at it, or up an arc that falls onto it (bombs just drop). A
    // silo weapon's missile leaves along its muzzle instead (a nuke rises
    // from its silo) and steers itself onto its target: its blueprint's
    // acceleration and turn rate and its script fly it.
    Vector3 vel;
    f32 flight_time = 0;
    const bool arcs = ballistic_arc != Arc::None && !need_compute_bomb_drop;
    Vector3 facing{};
    if (counted_projectile) {
        facing = muzzle_dir.value_or(Vector3{0.0f, 1.0f, 0.0f});
        const f32 len = std::sqrt(facing.x * facing.x + facing.y * facing.y + facing.z * facing.z);
        facing = len > 0.001f ? Vector3{facing.x / len, facing.y / len, facing.z / len}
                              : Vector3{0.0f, 1.0f, 0.0f};
        vel = {facing.x * muzzle_velocity, facing.y * muzzle_velocity, facing.z * muzzle_velocity};
    } else if (arcs) {
        const f32 elevation = launch_elevation(dist, dy);
        const f32 across = muzzle_velocity * osc::dmath::cos(elevation);
        vel = {dx / dist * across, muzzle_velocity * osc::dmath::sin(elevation),
               dz / dist * across};
        flight_time = across > 0.001f ? dist / across : 0.0f;
    } else if (need_compute_bomb_drop) {
        vel = {dx / dist * muzzle_velocity, 0.0f, dz / dist * muzzle_velocity};
    } else {
        const f32 span = std::sqrt(dist * dist + dy * dy);
        vel = {dx / span * muzzle_velocity, dy / span * muzzle_velocity,
               dz / span * muzzle_velocity};
        flight_time = muzzle_velocity > 0 ? span / muzzle_velocity : 0.0f;
    }

    // Create projectile
    auto proj = std::make_unique<Projectile>();
    proj->set_position(spawn_pos);
    proj->set_army(owner.army());
    proj->velocity = vel;
    proj->target_entity_id = (need_compute_bomb_drop || !target) ? 0 : target->entity_id();
    proj->target_position = aim;
    proj->has_target_position = true;
    proj->launcher_id = owner.entity_id();
    proj->damage_amount = damage * owner.damage_multiplier();
    proj->damage_radius = damage_radius;
    proj->damage_type = damage_type;
    // Bombs drop from altitude so need more time; normal projectiles use flight time.
    // A missile's flight can't be foretold: a minute outlasts any tactical
    // missile's (strategic ones give their own Lifetime).
    proj->lifetime = counted_projectile ? 60.0f
                     : (need_compute_bomb_drop || muzzle_velocity <= 0)
                         ? 10.0f // generous for high-altitude drops
                         : flight_time + 2.0f;

    // Set projectile blueprint for rendering
    if (!projectile_bp_id.empty()) {
        proj->set_blueprint_id(projectile_bp_id);
    }

    const Projectile::BlueprintPhysics physics = proj->apply_blueprint_physics(L);
    if (physics.lifetime) proj->lifetime = *physics.lifetime;
    if (counted_projectile) {
        // Facing along its muzzle, even at rest: it accelerates that way.
        const f32 across = std::sqrt(facing.x * facing.x + facing.z * facing.z);
        proj->set_orientation(euler_to_quat(osc::dmath::atan2(facing.x, facing.z),
                                            osc::dmath::atan2(-facing.y, across), 0.0f));
    } else {
        f32 heading = osc::dmath::atan2(vel.x, vel.z);
        proj->set_orientation(euler_to_quat(heading, 0.0f, 0.0f));
    }

    // An arc is gravity's: its shot falls whatever its blueprint says. A
    // straight shot falls only if its blueprint says so, and a bomb unless it
    // says not (none of retail's does: they fall at Moho's default).
    if (arcs || (need_compute_bomb_drop ? physics.use_gravity.value_or(true)
                                        : physics.use_gravity.value_or(false)))
        proj->ballistic_accel = -kGravity;
    proj->in_water = in_water;
    u32 proj_id = registry.register_entity(std::move(proj));
    auto* proj_ptr = static_cast<Projectile*>(registry.find(proj_id));
    // The launch order it fired for is done.
    if (manual_fire) {
        if (UnitCommand* order = owner.launch_order_for(*this)) order->launched = true;
    }
    if (proj_ptr) create_projectile_object(L, *proj_ptr, in_water, false);
    return proj_ptr;
}

} // namespace osc::sim
