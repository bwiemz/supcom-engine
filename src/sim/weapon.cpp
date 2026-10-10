#include "sim/weapon.hpp"
#include "sim/flight_math.hpp"
#include "sim/projectile_script.hpp"
#include "core/dmath.hpp"
#include "core/test_status.hpp"
#include "map/terrain.hpp"
#include "sim/air_combat.hpp"
#include "sim/bone_data.hpp"
#include "sim/collision.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/projectile.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

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

// Moho's PredictInterceptPointConstantSpeed. Retail takes the lead term as
// |v| d sin(pi/2 - cos), not |v| d cos; kept as retail has it.
Vector3 intercept_at_speed(const Vector3& at, const Vector3& v, f32 speed, const Vector3& muzzle) {
    const f32 v_len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (v_len < 0.001f) {
        return at;
    }
    const Vector3 d{muzzle.x - at.x, muzzle.y - at.y, muzzle.z - at.z};
    const f32 dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (dist * dist < 0.0001f) {
        return at;
    }
    const f32 cos_angle =
        (v.x / v_len) * (d.x / dist) + (v.y / v_len) * (d.y / dist) + (v.z / v_len) * (d.z / dist);
    const f32 speed_delta = v_len * v_len - speed * speed;
    if (speed_delta * speed_delta < 0.0001f) {
        return at;
    }
    const f32 a = kPi * 0.5f - cos_angle;
    const f32 a2 = a * a;
    const f32 term = ((a2 * 0.00761f - 0.16605f) * a2 + 1.0f) * a * dist * v_len;
    const f32 disc = term * term - speed_delta * dist * dist;
    if (disc < 0.0f) {
        return at;
    }
    const f32 time = (term - std::sqrt(disc)) / speed_delta;
    return {at.x + v.x * time, at.y + v.y * time, at.z + v.z * time};
}

// Moho's PredictInterceptPointFromForwardVelocity.
Vector3 intercept_across(const Vector3& at, const Vector3& v, f32 across, const Vector3& muzzle) {
    if (across <= 0.001f) {
        return at;
    }
    const f32 inv = 1.0f / across;
    const auto time_to = [&](f32 x, f32 z) {
        return std::sqrt((muzzle.x - x) * (muzzle.x - x) + (muzzle.z - z) * (muzzle.z - z)) * inv;
    };
    f32 time = time_to(at.x, at.z);
    for (int i = 0; i < 10; ++i) {
        const f32 previous = time;
        time = time_to(at.x + v.x * time, at.z + v.z * time);
        if (std::fabs(time - previous) <= 0.1f) {
            break;
        }
    }
    return {at.x + v.x * time, at.y + v.y * time, at.z + v.z * time};
}

i32 find_muzzle_bone(const Unit& owner, const std::string& name) {
    if (name.empty() || !owner.bone_data()) {
        return -1;
    }
    return owner.bone_data()->find_bone(name);
}

Vector3 owner_facing(const Unit& owner) {
    return quat_rotate(owner.orientation(), Vector3{0.0f, 0.0f, 1.0f});
}

bool has_omni_detection(const Unit& owner, const Entity& target, const SimState* sim) {
    if (!sim || owner.army() < 0) return false;
    return (sim->recon_of(target, static_cast<u32>(owner.army())) & SimState::kReconOmni) != 0;
}

bool is_weapon_targetable(const Unit& owner, const Entity& target, const SimState* sim) {
    if (!target.is_unit()) return true;
    const auto* target_unit = static_cast<const Unit*>(&target);
    if (target_unit->is_cloaked() && !has_omni_detection(owner, target, sim)) {
        return false;
    }
    return true;
}

bool is_underwater(const std::string& layer) {
    return layer == "Sub" || layer == "Seabed";
}

/// The water's surface, as Moho's target points compare to it: -10000 on a
/// map without water (or with no terrain).
/// `d` (of unit length) turned `heading` across and `pitch` up in its own
/// frame, radians: the launch jitter of Moho's CreateProjectile.
Vector3 turned(const Vector3& d, f32 heading, f32 pitch) {
    const Vector3 up0 = std::fabs(d.y) < 0.99f ? Vector3{0, 1, 0} : Vector3{1, 0, 0};
    Vector3 side{up0.y * d.z - up0.z * d.y, up0.z * d.x - up0.x * d.z, up0.x * d.y - up0.y * d.x};
    const f32 sl = std::sqrt(side.x * side.x + side.y * side.y + side.z * side.z);
    side = {side.x / sl, side.y / sl, side.z / sl};
    const Vector3 up{d.y * side.z - d.z * side.y, d.z * side.x - d.x * side.z,
                     d.x * side.y - d.y * side.x};
    const f32 ch = osc::dmath::cos(heading);
    const f32 sh = osc::dmath::sin(heading);
    const f32 cp = osc::dmath::cos(pitch);
    const f32 sp = osc::dmath::sin(pitch);
    return {cp * (ch * d.x + sh * side.x) + sp * up.x, cp * (ch * d.y + sh * side.y) + sp * up.y,
            cp * (ch * d.z + sh * side.z) + sp * up.z};
}

f32 water_surface(const SimState* sim) {
    const map::Terrain* terrain = sim ? sim->terrain() : nullptr;
    return terrain && terrain->has_water() ? terrain->water_elevation() : -10000.0f;
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
    shooters.erase(
        std::remove_if(shooters.begin(), shooters.end(),
                       [&](const std::pair<u32, i32>& s) {
                           const Entity* e = registry.find(s.first);
                           if (!e || e->destroyed() || !e->is_unit()) return true;
                           const auto& weapons = static_cast<const Unit*>(e)->weapons();
                           return s.second < 0 || static_cast<size_t>(s.second) >= weapons.size() ||
                                  weapons[static_cast<size_t>(s.second)]->target_entity_id !=
                                      proj.entity_id();
                       }),
        shooters.end());
    return shooters.size() < cap;
}

/// The entities within `reach` of (x, z) a weapon may pick from, in id
/// order. One that shoots units takes the grid of units alone, which a
/// map's props and the projectiles in flight aren't in; one that shoots
/// projectiles takes every entity (can_pick keeps the projectiles).
std::vector<u32> target_candidates(const EntityRegistry& registry, bool projectiles, f32 x, f32 z,
                                   f32 reach) {
    if (projectiles) return registry.collect_in_radius(x, z, reach);
    const std::vector<Entity*> units = registry.units_in_radius(x, z, reach);
    std::vector<u32> ids;
    ids.reserve(units.size());
    for (const Entity* unit : units) ids.push_back(unit->entity_id());
    return ids;
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
    if (!enabled || !has_target() || owner.busy() || owner.is_stunned()) return false;
    if (counted_projectile && owner.silo_ammo(nuke_weapon) <= 0) return false;
    if (above_water_fire_only && is_underwater(owner.layer())) return false;
    // A script callback earlier this tick may have destroyed the target.
    const std::optional<Vector3> at = target_point(registry);
    if (!at) return false;
    // Tracked from farther (TrackingRadius), fired at only within MaxRadius,
    // and only once the fire control is on target.
    if (!in_firing_range(owner, *at)) return false;
    // Only within its heading arc (Moho's fire task: the solution is
    // Available), a slaved weapon's target behind it, or a ground target.
    if (!in_heading_arc(owner, *at)) return false;
    if (const AimManipulator* aim = fire_control(owner);
        aim && !(aim->enabled() && aim->on_target()))
        return false;
    if (!winged_speed_ok(owner)) return false;
    if (need_compute_bomb_drop && !bomb_ready(owner, *at, registry)) return false;
    return true;
}

bool Weapon::winged_speed_ok(const Unit& owner) const {
    if (!auto_initiate_attack_command || !owner.air_combat_rules().winged) return true;
    const Vector3& v = owner.velocity();
    const f32 speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return speed >= owner.max_airspeed() * owner.speed_mult() * 0.25f;
}

bool Weapon::bomb_ready(const Unit& owner, const Vector3& at,
                        const EntityRegistry& registry) const {
    const AirCombatRules& air = owner.air_combat_rules();
    if (!air.winged) return true; // Moho computes no drop for a hovering flier
    if (!owner.has_unit_state("MakingAttackRun")) return false;
    // At a unit, its target point; where a moving one will be, for a bomber
    // that predicts ahead (Moho's CanFire).
    Vector3 aim = at;
    const Entity* target = target_entity_id != 0 ? registry.find(target_entity_id) : nullptr;
    if (target && target->is_unit()) {
        const auto& unit = static_cast<const Unit&>(*target);
        if (air.predict_ahead_for_bomb_drop > 0.0f && unit.is_mobile()) {
            const Vector3& v = unit.velocity();
            aim.x += v.x * air.predict_ahead_for_bomb_drop;
            aim.z += v.z * air.predict_ahead_for_bomb_drop;
        } else {
            aim = unit.target_point(current_aim_spot());
        }
    }
    const std::optional<Vector3> release =
        calc_bomb_drop(owner.velocity(), owner.position(), aim, kGravity);
    return release &&
           bomb_release_ok(owner.position(), owner.heading(), *release, aim, bomb_drop_threshold);
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

void Weapon::update(Unit& owner, EntityRegistry& registry, lua_State* L, const SimState* sim) {
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
        update_targeting(owner, registry, sim);
    }
    if (aim_spot_target != target_entity_id) pick_aim_spot(registry, sim);
    update_aim(owner, registry, L);
    if (owner.destroyed() || owner.is_dying()) return; // a tracking callback may kill it

    if (L && fires_through_script()) {
        update_scripted(owner, registry, L, previous);
        return;
    }

    if (!has_target() || fire_clock > 0 || ground_fire_barred()) return;
    const std::optional<Vector3> at = target_point(registry);
    if (!at || !in_firing_range(owner, *at) || !in_heading_arc(owner, *at)) return;
    if (counted_projectile && owner.silo_ammo(nuke_weapon) <= 0) return;
    if (const AimManipulator* aim = fire_control(owner);
        aim && !(aim->enabled() && aim->on_target()))
        return;
    if (try_fire(owner, registry, L, sim)) {
        fire_clock = fire_period();
        ++shots_at_target;
    }
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

void Weapon::pick_aim_spot(EntityRegistry& registry, const SimState* sim) {
    aim_spot_target = target_entity_id;
    aim_spot = -1;
    const Entity* target = target_entity_id != 0 ? registry.find(target_entity_id) : nullptr;
    if (!target || !target->is_unit()) return;
    const auto& unit = static_cast<const Unit&>(*target);
    if (above_water_targets_only || below_water_targets_only) {
        unit.pick_target_point_by_water(&registry.sim_random(), water_surface(sim),
                                        above_water_targets_only, aim_spot);
        return;
    }
    aim_spot = unit.pick_target_point(registry.sim_random());
}

void Weapon::drop_target(lua_State* L) {
    const bool had = has_target();
    set_target_entity(0);
    if (had && fires_through_script()) call_script(L, "OnLostTarget");
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
    if (fire_clock > 0 || ground_fire_barred() || !can_fire(owner, registry)) return;
    if (!call_script(L, "CanWeaponFire") || !still_firing()) return;
    call_script(L, "OnFire");
    fire_clock = fire_period();
    ++shots_at_target;
}

bool Weapon::can_pick(const Unit& owner, const Entity& target, const SimState* sim) const {
    // A weapon shoots units, or, with TargetType RULEWTT_Projectile, the
    // other side's projectiles (M206b).
    if (target.destroyed() || target.entity_id() == owner.entity_id()) return false;
    if (targets_projectiles ? !target.is_projectile() : !target.is_unit()) return false;
    if (target.do_not_target() || target.army() < 0) return false;
    if (sim ? !sim->is_enemy(owner.army(), target.army()) : target.army() == owner.army())
        return false;
    if (target.is_unit()) {
        if (!is_weapon_targetable(owner, target, sim)) return false;
        const auto& unit = static_cast<const Unit&>(target);
        if (fire_target_layer_caps != 0xFF &&
            !(layer_to_bit(unit.layer()) & fire_target_layer_caps))
            return false;
        // A unit on the seabed only where a target point of it is on the
        // weapon's side of the surface: a tall walker in the shallows is in
        // reach of guns above the water (Moho's CanAttackTarget). Other
        // layers are the layer caps' alone.
        if (unit.layer() == "Seabed" && (above_water_targets_only || below_water_targets_only)) {
            i32 unused = -1;
            if (!unit.pick_target_point_by_water(nullptr, water_surface(sim),
                                                 above_water_targets_only, unused))
                return false;
        }
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
    return (restrict_only_allow.empty() || restrict_only_allow.matches(categories)) &&
           !restrict_disallow.matches(categories);
}

bool Weapon::can_target(const Unit& owner, const Entity& target, const SimState* sim,
                        bool in_reach) const {
    if (!can_pick(owner, target, sim)) {
        return false;
    }
    const f32 dx = target.position().x - owner.position().x;
    const f32 dz = target.position().z - owner.position().z;
    const f32 dist2 = dx * dx + dz * dz;
    const f32 reach = max_range * std::max(1.0f, tracking_radius);
    if (in_reach && (dist2 > reach * reach || dist2 < min_range * min_range)) return false;
    // Only targets within the arc about the unit's facing; a mobile unit's
    // slaved weapon may hold one outside it, which its hull turns to (Moho's
    // FindBestEnemy keeps NoSolution candidates for it).
    if (!(slaved_to_body && owner.is_mobile()) && !in_heading_arc(owner, target.position()))
        return false;
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

void Weapon::update_targeting(Unit& owner, EntityRegistry& registry, const SimState* sim) {
    // A unit that unpacks to fire looks for nothing while a move takes it
    // somewhere, or while it boards a transport (Moho's CAcquireTargetTask:
    // the Moving, TransportLoading and WaitingForTransport states).
    const auto& queue = owner.command_queue();
    const CommandType head = queue.empty() ? CommandType::Stop : queue.front().type;
    if (owner.need_unpack() && (head == CommandType::Move || head == CommandType::TransportLoad ||
                                head == CommandType::WaitForFerry))
        return;
    const bool flier = owner.layer() == "Air";
    // A ground attack order, once its unit is in reach (Moho's attack task
    // has set the attacker's desired target): each weapon that can hit the
    // ground there takes the point within its own range, or at any range on
    // an aircraft (CAcquireTargetTask), and keeps it while the order lasts.
    const UnitCommand* order = queue.empty() ? nullptr : &queue.front();
    if (order && order->type == CommandType::Attack && order->target_id == 0 && order->engaged &&
        attacks_on_order() &&
        can_attack_ground(order->target_pos, sim ? sim->terrain() : nullptr) &&
        (flier || in_firing_range(owner, order->target_pos))) {
        set_target_ground(order->target_pos);
        ground_from_order = true;
        return;
    }
    // The order gone, or the point out of this weapon's reach: it looks for
    // targets again.
    if (ground_from_order) set_target_entity(0);
    // An aircraft's weapons take its ordered target at any range: Moho's
    // CAcquireTargetTask gives a flier the attacker's desired target out of
    // reach too, so it is kept through the runs' loops.
    const u32 ordered = attack_order_target(owner);
    // A target this weapon can no longer shoot is dropped at once.
    if (target_entity_id != 0) {
        const Entity* target = registry.find(target_entity_id);
        const bool in_reach = !(flier && target_entity_id == ordered);
        if (!target || !can_target(owner, *target, sim, in_reach)) target_entity_id = 0;
    }

    // An attack order's target comes first, for every weapon that can hit
    // it, whatever the priorities say.
    if (ordered != 0) {
        if (ordered == target_entity_id) return;
        const Entity* target = registry.find(ordered);
        if (target && can_target(owner, *target, sim, !flier)) {
            set_target_entity(ordered);
            return;
        }
    }

    // A ground target its script set stays until the script changes it.
    if (has_ground_target) return;
    // Attacking, a unit that unpacks to fire takes its ordered target only.
    if (owner.need_unpack() && head == CommandType::Attack) return;

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
    bool best_in_arc = false;
    f32 best_dist2 = 0;
    const f32 reach = max_range * std::max(1.0f, tracking_radius);
    static const CategoryName kBenign{"BENIGN"};
    for (const u32 id : target_candidates(registry, targets_projectiles, owner.position().x,
                                          owner.position().z, reach)) {
        Entity* e = registry.find(id);
        if (!e || !can_target(owner, *e, sim)) continue;
        if (e->is_unit() && static_cast<const Unit&>(*e).has_category(kBenign)) {
            continue;
        }
        // A missile already held by as many weapons as it wants shooting at
        // it (a nuke's DesiredShooterCap is 1) is left to them.
        if (e->is_projectile() && !shooter_room(static_cast<Projectile&>(*e), registry)) continue;
        const int priority = priority_of(*e);
        if (priority < 0) continue;
        const f32 dx = e->position().x - owner.position().x;
        const f32 dz = e->position().z - owner.position().z;
        const f32 dist2 = dx * dx + dz * dz;
        // Within a priority, one in its heading arc beats one out of it (a
        // slaved weapon's; Moho's FindBestEnemy penalises NoSolution).
        const bool in_arc = in_heading_arc(owner, e->position());
        const bool better_arc = in_arc && !best_in_arc;
        if (best_id == 0 || priority < best_priority ||
            (priority == best_priority &&
             (better_arc || (in_arc == best_in_arc && dist2 < best_dist2)))) {
            best_id = id;
            best_priority = priority;
            best_in_arc = in_arc;
            best_dist2 = dist2;
        }
    }
    // A recheck changes target only for a better priority.
    if (current_priority >= 0 && (best_id == 0 || best_priority >= current_priority)) return;
    if (best_id == 0) return;
    target_entity_id = best_id;
    // An idle aircraft's weapon with AutoInitiateAttackCommand makes the pick
    // its unit's attack order (Moho's CAcquireTargetTask, CheckAutoInitiate).
    if (auto_initiate_attack_command && owner.is_mobile() && owner.auto_initiate_allowed(registry))
        owner.request_auto_attack(best_id);
    if (Entity* target = registry.find(best_id); target && target->is_projectile())
        static_cast<Projectile&>(*target).shooters.emplace_back(owner.entity_id(), weapon_index);
}

bool Weapon::in_firing_range(const Unit& owner, const Entity& target) const {
    return in_firing_range(owner, target.position());
}

bool Weapon::in_heading_arc(const Unit& owner, const Vector3& at) const {
    if (heading_arc_range >= 180.0f) return true;
    const Vector3 forward = quat_rotate(owner.orientation(), Vector3{0.0f, 0.0f, 1.0f});
    constexpr f32 kDegToRad = 3.14159265358979f / 180.0f;
    f32 off = osc::dmath::atan2(at.x - owner.position().x, at.z - owner.position().z) -
              osc::dmath::atan2(forward.x, forward.z) - heading_arc_center * kDegToRad;
    while (off > 3.14159265f) off -= 6.28318531f;
    while (off < -3.14159265f) off += 6.28318531f;
    return std::fabs(off) <= heading_arc_range * kDegToRad;
}

bool Weapon::can_attack_ground(const Vector3& at, const map::Terrain* terrain) const {
    if (cannot_attack_ground) return false;
    u8 layer = layer_to_bit("Land");
    if (terrain) {
        // Moho's water elevation on a map without water is -10000.
        const f32 ground = terrain->get_terrain_height(at.x, at.z);
        const f32 water = terrain->has_water() ? terrain->water_elevation() : -10000.0f;
        if (ground > water) layer = layer_to_bit("Land");
        else if (water > ground) layer = layer_to_bit("Water");
        else return false;
    }
    return (fire_target_layer_caps & layer) != 0;
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
            const i32 bone = find_muzzle_bone(owner, muzzle_bone_name);
            const Vector3 from = bone >= 0 ? owner.bone_world_position(bone) : owner.position();
            const Vector3 at =
                target ? aim_point(*target, owner, from,
                                   bone >= 0 ? owner.bone_world_forward(bone) : owner_facing(owner))
                       : ground_target;
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

bool Weapon::try_fire(Unit& owner, EntityRegistry& registry, lua_State* L, const SimState* sim) {
    // At a unit, or at its ground target.
    auto* target = target_entity_id != 0 ? registry.find(target_entity_id) : nullptr;
    if (target_entity_id != 0 &&
        (!target || target->destroyed() || target->do_not_target() ||
         !is_weapon_targetable(owner, *target, sim) ||
         (fire_target_layer_caps != 0xFF && target->is_unit() &&
          !(layer_to_bit(static_cast<Unit*>(target)->layer()) & fire_target_layer_caps)))) {
        target_entity_id = 0;
        return false;
    }
    if (!target && !has_ground_target) return false;
    const Vector3 at = target ? target->position() : ground_target;

    // A bomb only at its release point (bomb_ready); a winged auto-attacker
    // only under way.
    if (!winged_speed_ok(owner)) return false;
    if (need_compute_bomb_drop && !bomb_ready(owner, at, registry)) return false;

    // Resolve muzzle bone position for projectile spawn
    // From the muzzle as the turret is posed now.
    Vector3 spawn_pos = owner.position();
    std::optional<Vector3> muzzle_dir;
    if (const i32 bone = find_muzzle_bone(owner, muzzle_bone_name); bone >= 0) {
        spawn_pos = owner.bone_world_position(bone);
        muzzle_dir = owner.bone_world_forward(bone);
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

f32 Weapon::muzzle_speed_at(f32 distance, SimRandom* rng) const {
    f32 speed = muzzle_velocity;
    if (rng && muzzle_velocity_random != 0.0f) speed += gaussian(*rng) * muzzle_velocity_random;
    if (muzzle_velocity_reduce_distance > distance)
        speed *= std::sqrt(distance / muzzle_velocity_reduce_distance);
    return speed;
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
    // At the speed a shot that far leaves at (MuzzleVelocityReduceDistance
    // slows it close in), so the barrel's arc is the shell's.
    const f32 speed = muzzle_speed_at(std::sqrt(dist * dist + rise * rise), nullptr);
    const f32 v2 = speed * speed;
    const f32 disc = v2 * v2 - kGravity * (kGravity * dist * dist + 2.0f * rise * v2);
    if (speed <= 0 || disc < 0) return kPi * 0.25f;
    const f32 root = std::sqrt(disc);
    return osc::dmath::atan2(ballistic_arc == Arc::High ? v2 + root : v2 - root, kGravity * dist);
}

Vector3 Weapon::aim_point(const Entity& target, const Unit& owner, const Vector3& muzzle,
                          const Vector3& muzzle_forward) const {
    constexpr f32 kTick = 0.1f;
    Vector3 v{};
    if (target.is_unit()) {
        v = static_cast<const Unit&>(target).velocity();
    } else if (target.is_projectile()) {
        v = static_cast<const Projectile&>(target).velocity;
    }
    // On a unit, its target point (Moho's GetTargetPosGun); a projectile's
    // middle.
    const Vector3 centre = target.is_unit()
                               ? static_cast<const Unit&>(target).target_point(current_aim_spot())
                               : collision_centre(target);
    const Vector3 at{centre.x + v.x * kTick, centre.y + v.y * kTick, centre.z + v.z * kTick};
    if (!lead_target) {
        return at;
    }
    Vector3 from = muzzle;
    if (owner.is_mobile()) {
        const Vector3& own = owner.velocity();
        from = {from.x + own.x * kTick, from.y + own.y * kTick, from.z + own.z * kTick};
    }
    const f32 across =
        std::sqrt((from.x - at.x) * (from.x - at.x) + (from.z - at.z) * (from.z - at.z));
    f32 speed = muzzle_velocity;
    if (muzzle_velocity_reduce_distance > across) {
        speed = std::sqrt(across / muzzle_velocity_reduce_distance) * muzzle_velocity;
    }
    if (projectile_physics && projectile_physics->track_target) {
        return intercept_at_speed(at, v, projectile_physics->max_speed, from);
    }
    if (projectile_physics && projectile_physics->use_gravity) {
        const f32 level =
            std::sqrt(muzzle_forward.x * muzzle_forward.x + muzzle_forward.z * muzzle_forward.z);
        return intercept_across(at, v, level * speed, from);
    }
    return intercept_at_speed(at, v, speed, from);
}

Projectile* Weapon::launch(Unit& owner, const Vector3& spawn_pos, const Entity* target,
                           EntityRegistry& registry, lua_State* L, bool in_water,
                           std::optional<Vector3> muzzle_dir) {
    // Where the shot goes: the target (where it will be, for a weapon that
    // leads), its ground target, where a manual weapon's last order sent it,
    // or along the owner's facing to its reach with none.
    Vector3 aim;
    if (target) {
        aim = aim_point(*target, owner, spawn_pos, muzzle_dir.value_or(owner_facing(owner)));
    } else if (has_ground_target) {
        aim = ground_target;
    } else if (manual_fire && last_order_point) {
        aim = *last_order_point;
    } else {
        const Vector3 forward = owner_facing(owner);
        const f32 len = std::sqrt(forward.x * forward.x + forward.z * forward.z);
        const f32 reach = max_range > 0 ? max_range : 10.0f;
        aim = len > 0.001f ? Vector3{spawn_pos.x + forward.x / len * reach, spawn_pos.y,
                                     spawn_pos.z + forward.z / len * reach}
                           : Vector3{spawn_pos.x, spawn_pos.y, spawn_pos.z + reach};
    }
    const Vector3 unscattered = aim; // a bomb's (RealisticOrdinance, below)
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
    // The speed it leaves at, for its target's distance (Moho's
    // GetMuzzleVelocity, drawing MuzzleVelocityRandom).
    const f32 speed = muzzle_speed_at(std::sqrt(dist * dist + dy * dy), &registry.sim_random());
    Vector3 facing{};
    if (counted_projectile) {
        facing = muzzle_dir.value_or(Vector3{0.0f, 1.0f, 0.0f});
        const f32 len = std::sqrt(facing.x * facing.x + facing.y * facing.y + facing.z * facing.z);
        facing = len > 0.001f ? Vector3{facing.x / len, facing.y / len, facing.z / len}
                              : Vector3{0.0f, 1.0f, 0.0f};
        vel = {facing.x * muzzle_velocity, facing.y * muzzle_velocity, facing.z * muzzle_velocity};
    } else if (arcs) {
        const f32 elevation = launch_elevation(dist, dy);
        const f32 across = speed * osc::dmath::cos(elevation);
        vel = {dx / dist * across, speed * osc::dmath::sin(elevation), dz / dist * across};
        flight_time = across > 0.001f ? dist / across : 0.0f;
    } else if (need_compute_bomb_drop) {
        vel = {dx / dist * muzzle_velocity, 0.0f, dz / dist * muzzle_velocity};
    } else {
        const f32 span = std::sqrt(dist * dist + dy * dy);
        vel = {dx / span * speed, dy / span * speed, dz / span * speed};
        flight_time = speed > 0 ? span / speed : 0.0f;
    }
    // Which way it leaves (Moho's CreateProjectile): above is its firing
    // solution, but a shell leaves along its muzzle as the aim controller
    // has posed it -- a turret still turning, or one within its
    // FiringTolerance, misses by as much -- unless its weapon fires along
    // its solution (UseFiringSolutionInsteadOfAimBone) or it drops straight
    // down (StraightDownOrdinance). Then FiringRandomness r turns it by a
    // heading and a pitch each drawn N(0, r) degrees. Silo missiles and
    // bombs keep their own rules.
    if (!counted_projectile && !need_compute_bomb_drop) {
        const f32 vl = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
        Vector3 dir = vl > 0.0f ? Vector3{vel.x / vl, vel.y / vl, vel.z / vl} : owner_facing(owner);
        if (projectile_physics && projectile_physics->straight_down) {
            dir = {0.0f, -1.0f, 0.0f};
        } else if (!use_firing_solution && muzzle_dir) {
            const Vector3& m = *muzzle_dir;
            const f32 ml = std::sqrt(m.x * m.x + m.y * m.y + m.z * m.z);
            if (ml > 0.001f) dir = {m.x / ml, m.y / ml, m.z / ml};
        }
        if (firing_randomness > 0.0f) {
            constexpr f32 kDegToRad = kPi / 180.0f;
            const f32 heading = gaussian(registry.sim_random()) * firing_randomness * kDegToRad;
            const f32 pitch = gaussian(registry.sim_random()) * firing_randomness * kDegToRad;
            dir = turned(dir, heading, pitch);
        }
        vel = {dir.x * speed, dir.y * speed, dir.z * speed};
    }

    // Create projectile
    auto proj = std::make_unique<Projectile>();
    proj->set_position(spawn_pos);
    proj->set_army(owner.army());
    proj->velocity = vel;
    proj->target_entity_id = (need_compute_bomb_drop || !target) ? 0 : target->entity_id();
    // It homes on the point its weapon aimed at (Moho copies the target in).
    proj->target_point = proj->target_entity_id != 0 ? current_aim_spot() : -1;
    proj->target_position = aim;
    // Sent somewhere: at a target, a ground target or a manual order's
    // point. Fired along its facing with none, it has no target (Moho's
    // weapon target is None then), which a homing shot can't fly with.
    proj->has_target_position =
        target != nullptr || has_ground_target || (manual_fire && last_order_point);
    proj->launcher_id = owner.entity_id();
    proj->damage_amount = damage;
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

    const Projectile::BlueprintPhysics physics =
        proj->apply_blueprint_physics(L, &registry.sim_random());
    if (physics.lifetime) proj->lifetime = *physics.lifetime;
    if (physics.realistic_ordinance) {
        // A bomb (RealisticOrdinance) leaves with its launcher's speed,
        // aimed flat from the launcher at its target -- or where a moving
        // one will be, by the launcher's PredictAheadForBombDrop -- as
        // Moho's Projectile does; firing randomness doesn't move it.
        Vector3 at = unscattered;
        const f32 ahead = owner.air_combat_rules().predict_ahead_for_bomb_drop;
        if (ahead > 0.0f && target && target->is_unit() &&
            static_cast<const Unit*>(target)->is_mobile())
            at = predict_ahead(*target, ahead);
        const Vector3& v = owner.velocity();
        const f32 speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        const f32 ax = at.x - owner.position().x;
        const f32 az = at.z - owner.position().z;
        const f32 len = std::sqrt(ax * ax + az * az);
        vel = len > 0.001f ? Vector3{ax / len * speed, 0.0f, az / len * speed}
                           : Vector3{v.x, 0.0f, v.z};
        proj->velocity = vel;
        proj->target_position = at;
    }
    // The weapon's own lifetime for its shots wins (an anti-torpedo's
    // blueprint says half a second, its weapon four).
    if (projectile_lifetime_multiplier > 0 && muzzle_velocity > 0)
        proj->lifetime = projectile_lifetime_multiplier * max_range / muzzle_velocity;
    else if (projectile_lifetime > 0) proj->lifetime = projectile_lifetime;
    if (counted_projectile) {
        // Facing along its muzzle, even at rest: it accelerates that way.
        const f32 across = std::sqrt(facing.x * facing.x + facing.z * facing.z);
        proj->set_orientation(euler_to_quat(osc::dmath::atan2(facing.x, facing.z),
                                            osc::dmath::atan2(-facing.y, across), 0.0f));
    } else {
        // Facing the way it leaves: it thrusts and turns from there (Moho's
        // launch transform)
        proj->set_orientation(coords_orient(vel));
    }
    proj->arm_lost_target_aim(registry);

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
