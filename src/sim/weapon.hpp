#pragma once

#include "core/types.hpp"
#include "sim/category_expr.hpp"
#include "sim/entity.hpp" // Vector3

#include <optional>
#include <string>
#include <vector>

struct lua_State;

namespace osc::map {
class Terrain;
}

namespace osc::sim {

class AimManipulator;
class EntityRegistry;
class Projectile;
class SimState;
class Unit;

class Weapon {
public:
    // Blueprint data (cached from Lua bp table at creation)
    std::string label;
    f32 max_range = 0;
    f32 min_range = 0;
    f32 rate_of_fire = 1;       // shots per second
    f32 damage = 0;
    f32 damage_radius = 0;
    std::string damage_type = "Normal";
    f32 muzzle_velocity = 0;
    /// How long its shots live, overriding their projectile blueprint's
    /// Lifetime (FAF's notes on the engine): ProjectileLifetimeUsesMultiplier
    /// x MaxRadius / MuzzleVelocity, else ProjectileLifetime; 0 leaves it.
    f32 projectile_lifetime = 0;
    f32 projectile_lifetime_multiplier = 0;
    /// How a shot flies to its target (BallisticArc): straight, or on the
    /// low or the high of the two arcs gravity allows at its muzzle velocity.
    enum class Arc : u8 { None, Low, High };
    Arc ballistic_arc = Arc::None;
    bool lead_target = true;                 ///< LeadTarget: aim where a moving target will be
    f32 muzzle_velocity_reduce_distance = 0; // MuzzleVelocityReduceDistance
    struct ProjectilePhysics {
        bool track_target = false;
        bool use_gravity = true;
        f32 max_speed = 0;
    };
    std::optional<ProjectilePhysics> projectile_physics;

    /// The launch angle above the horizontal for a shot `dist` away and
    /// `rise` above: its arc's, at its muzzle velocity under gravity (45
    /// degrees, the furthest, when out of reach).
    f32 launch_elevation(f32 dist, f32 rise) const;
    /// Where to aim at `target` from a muzzle at `muzzle` facing
    /// `muzzle_forward`: Moho's CAimManipulator::Aim.
    Vector3 aim_point(const Entity& target, const Unit& owner, const Vector3& muzzle,
                      const Vector3& muzzle_forward) const;
    bool fire_on_death = false;
    /// DummyWeapon, or a Death weapon (a structure's explosion): never one
    /// a unit attacks with.
    bool dummy = false;
    /// ManualFire: fires only at what its unit's launch order names.
    bool manual_fire = false;
    /// CountedProjectile: fires missiles its unit stores and builds (M206).
    bool counted_projectile = false;
    bool nuke_weapon = false;        ///< NukeWeapon: its missiles are nukes, else tactical
    i32 max_projectile_storage = 0;  ///< MaxProjectileStorage: the silo builds up to this
    /// TargetType RULEWTT_Projectile: it shoots the other side's projectiles
    /// (missiles, torpedoes), never units (M206b).
    bool targets_projectiles = false;
    bool overcharge = false;         // OverChargeWeapon
    /// It fires collision beams (a DefaultBeamWeapon made them for it, M206c).
    bool beam = false;
    /// MaximumBeamLength: how far its beams reach (0: its MaxRadius).
    f32 max_beam_length = 0;
    std::string muzzle_bone_name; // from RackBones[1].MuzzleBones[1]
    f32 firing_randomness = 0;    // spread circle r x distance / 12 across
    uint8_t fire_target_layer_caps = 0xFF; // bitmask: default = all layers
    f32 max_height_diff = 0;               // MaxHeightDiff / ChangeMaxHeightDiff (<= 0: unlimited)
    f32 firing_tolerance = 0;              // FiringTolerance (degrees) / ChangeFiringTolerance
    f32 tracking_radius = 1;               // TrackingRadius: acquire out to MaxRadius * this
    f32 heading_arc_center = 0;            // HeadingArcCenter (degrees from the unit's facing)
    f32 heading_arc_range = 180;           // HeadingArcRange (degrees either side)
    /// SlavedToBody: the unit's hull turns to bring this weapon's target
    /// into its reach (Moho's CalcMoveCommon); it may hold a target outside
    /// its heading arc on a mobile unit, and fires once it is in it.
    bool slaved_to_body = false;
    f32 slaved_arc_range = 1.0f;           // SlavedToBodyArcRange (degrees; Moho's default 1)
    std::string projectile_bp_id;          // ChangeProjectileBlueprint
    std::string fire_control_label;   // SetFireControl: whose OnTarget gates firing
    bool need_compute_bomb_drop = false; // NeedToComputeBombDrop
    /// AutoInitiateAttackCommand: an idle aircraft attacks what this weapon
    /// picks (Moho's CAcquireTargetTask), and a winged one's fires only at
    /// a quarter of its top speed.
    bool auto_initiate_attack_command = false;
    f32 bomb_drop_threshold = 1.5f; // BombDropThreshold (Moho's default)
    // Targeting: SetTargetingPriorities (compiled; a candidate must match
    // one, and earlier ones win), the blueprint's restrictions (empty: none)
    // and how often targets are looked for.
    std::vector<CategoryExpr> target_priorities;
    CategoryExpr restrict_disallow;        // TargetRestrictDisallow
    CategoryExpr restrict_only_allow;      // TargetRestrictOnlyAllow
    bool above_water_targets_only = false; // AboveWaterTargetsOnly
    bool yaw_only_on_target = false;       // YawOnlyOnTarget: its aim controllers ignore the pitch
    bool above_water_fire_only = false;    // AboveWaterFireOnly
    bool always_recheck_target = true;     // AlwaysRecheckTarget
    u32 target_check_period = 30;          // TargetCheckInterval, in ticks
    bool cannot_attack_ground = false;     // CannotAttackGround
    /// AttackGroundTries: shots at a ground attack's point before the order
    /// goes to the back of the queue, when another follows (Moho's default 3)
    i32 attack_ground_tries = 3;
    int weapon_priorities_ref = -2;    // LUA_NOREF: SetWeaponPriorities Lua table ref
    int blueprint_ref = -2;     // LUA_NOREF = Lua registry ref to weapon bp table
    int lua_table_ref = -2;     // LUA_NOREF = Lua ref to weapon Lua table
    bool script_class = false;  // the Lua object is an instance of a weapon class
    i32 weapon_index = 0;       // 0-based index within unit
    u32 owner_entity_id = 0;    // back-pointer to owning unit

    // Runtime state
    u32 target_entity_id = 0;   // 0 = no target
    /// A point on the ground it is aimed at instead of a unit (SetTargetGround,
    /// a launch order). Only valid while target_entity_id is 0.
    bool has_ground_target = false;
    Vector3 ground_target;
    /// The ground target is its unit's ground attack order's (Moho's
    /// attacker's desired target): it goes when that order does. One its
    /// script set stays until the script changes it.
    bool ground_from_order = false;
    /// Shots since it last changed target (Moho's mShotsAtTarget): a
    /// ground attack gives way to the next order after AttackGroundTries.
    u32 shots_at_target = 0;
    /// Where a manual weapon's last order sent it. Its script may fire after
    /// the order is gone (a launch cancelled once the silo is opening); the
    /// missile then goes there.
    std::optional<Vector3> last_order_point;
    bool enabled = true;
    u32 fire_clock = 0; // ticks until the fire clock is ready again
    u32 target_check_clock = 0; // ticks until the next target scan

    /// Ticks between shots: 1/RateOfFire, rounded to whole ticks as Moho
    /// does (at least one).
    u32 fire_period() const;

    /// Retail's firing cycle drives this weapon: the engine picks targets and
    /// runs the fire clock, and the weapon's script state machine gets
    /// OnGotTarget/OnLostTarget/OnFire and fires its own racks and salvos (a
    /// silo weapon's script also takes its ammunition; a beam weapon's
    /// switches its beams on; an OverCharge weapon's draws its energy).
    bool fires_through_script() const { return script_class && lua_table_ref >= 0; }

    /// The weapon script's `method(self [, arg])`, if it has one. Returns
    /// its first result's truth (true when there is no such method).
    bool call_script(lua_State* L, const char* method, const char* arg = nullptr) const;

    bool has_target() const { return target_entity_id != 0 || has_ground_target; }
    /// Aim at a point on the ground (dropping any unit target). A new
    /// target starts its shot count again, as Moho's SetTarget does.
    void set_target_ground(const Vector3& at) {
        if (target_entity_id != 0 || !has_ground_target || ground_target.x != at.x ||
            ground_target.y != at.y || ground_target.z != at.z)
            shots_at_target = 0;
        target_entity_id = 0;
        has_ground_target = true;
        ground_target = at;
        ground_from_order = false;
    }
    /// Aim at a unit (0: at nothing), dropping any ground target.
    void set_target_entity(u32 id) {
        if (id != target_entity_id || has_ground_target) shots_at_target = 0;
        target_entity_id = id;
        has_ground_target = false;
        ground_from_order = false;
    }
    /// Moho's UnitWeapon::CanAttackTarget for a ground target: not
    /// CannotAttackGround, and the weapon hits the layer there (Land where
    /// the terrain is above the water, Water where it is under; neither
    /// where they meet). Without a terrain, the ground is land.
    bool can_attack_ground(const Vector3& at, const map::Terrain* terrain) const;
    /// `at` is within its HeadingArcRange about HeadingArcCenter from the
    /// unit's facing (always, with a range of 180 or more).
    bool in_heading_arc(const Unit& owner, const Vector3& at) const;
    /// The target is within MaxRadius (it may be tracked from farther) and
    /// outside MinRadius, horizontally.
    bool in_firing_range(const Unit& owner, const Entity& target) const;
    bool in_firing_range(const Unit& owner, const Vector3& at) const;
    /// A CannotAttackGround weapon aimed at the ground (its script's
    /// SetTargetGround) holds its fire, as Moho's fire task does.
    bool ground_fire_barred() const {
        return cannot_attack_ground && has_ground_target && target_entity_id == 0;
    }
    /// One of the weapons an attack order is carried out with: not a death,
    /// dummy or launch-order (ManualFire) weapon.
    bool attacks_on_order() const { return !fire_on_death && !dummy && !manual_fire; }
    /// Aim at nothing now, its script told OnLostTarget at once if it had a
    /// target (Moho's UnitWeapon::SetTarget with a cleared target).
    void drop_target(lua_State* L);
    /// Where it is aiming: its target unit's position or its ground target.
    /// Nothing without a target, or when its target is gone.
    std::optional<Vector3> target_point(const EntityRegistry& registry) const;

    /// Moho's CanFire: a target within MaxRadius, the weapon enabled, its
    /// fire control on target (see fire_control), the unit free (not Busy)
    /// and above water if it must be, a winged bomber at its release point
    /// (bomb_ready), and a silo weapon's missile ready (HasSiloAmmo).
    bool can_fire(const Unit& owner, const EntityRegistry& registry) const;

    /// A winged aircraft's bomb (NeedToComputeBombDrop): only in a run
    /// (MakingAttackRun), and only where a bomb dropped now, with the
    /// aircraft's velocity, lands on `at` -- or where a moving target will
    /// be, by its PredictAheadForBombDrop (Moho's UnitWeapon::CanFire).
    bool bomb_ready(const Unit& owner, const Vector3& at, const EntityRegistry& registry) const;
    /// A winged aircraft's AutoInitiateAttackCommand weapon fires only at a
    /// quarter of its top speed or more (Moho's UnitWeapon::CanFire).
    bool winged_speed_ok(const Unit& owner) const;

    /// Whether this weapon may shoot `target` from where `owner` stands: an
    /// enemy (by alliance, when `sim` is given), targetable, on a layer the
    /// weapon can hit, allowed by its restrictions, and in range. Range is a
    /// cylinder, as Moho's is: horizontal distance within the min and max
    /// radius, and height within MaxHeightDiff when that is set.
    /// Priorities are not checked: an attack order can pick any such unit.
    /// `in_reach` false leaves the range out: an aircraft's weapons keep its
    /// ordered target through its runs' loops.
    bool can_target(const Unit& owner, const Entity& target, const SimState* sim,
                    bool in_reach = true) const;
    /// can_target, wherever the target is.
    bool can_pick(const Unit& owner, const Entity& target, const SimState* sim) const;

    /// Index of the first priority `target` matches (0 when the weapon has
    /// none), or -1 when it matches none.
    int priority_of(const Entity& target) const;

    /// The aim controller whose OnTarget gates this weapon's fire: the one
    /// SetFireControl named, else the first created for it; null when the
    /// weapon has none (it then fires without aiming).
    const AimManipulator* fire_control(const Unit& owner) const;

    /// Per tick: advance the fire clock, pick targets, fire when ready.
    void update(Unit& owner, EntityRegistry& registry, lua_State* L,
                const SimState* sim = nullptr);

    /// Fire the weapon at current target. Returns true if fired.
    /// Fire one projectile from `spawn_pos` at `target` (when null: at its
    /// ground target, where a manual weapon's last order sent it, or along
    /// the owner's facing): the weapon's muzzle
    /// velocity and spread, the projectile blueprint's physics, the weapon's
    /// damage. A silo weapon's missile instead leaves along `muzzle_dir` (its
    /// muzzle bone's facing, when known) and steers itself. It is registered
    /// and given its script object (OnCreate runs), and a launch order the
    /// weapon serves is marked fired. Returns it.
    Projectile* launch(Unit& owner, const Vector3& spawn_pos, const Entity* target,
                       EntityRegistry& registry, lua_State* L, bool in_water,
                       std::optional<Vector3> muzzle_dir = std::nullopt);
    bool try_fire(Unit& owner, EntityRegistry& registry, lua_State* L,
                  const SimState* sim = nullptr);

private:
    void update_targeting(Unit& owner, EntityRegistry& registry, const SimState* sim);
    /// What it was aiming at: the script hears when that changes.
    struct TargetMark {
        u32 entity = 0;
        bool ground = false;
        Vector3 point;
        bool operator==(const TargetMark& o) const {
            return entity == o.entity && ground == o.ground &&
                   (!ground ||
                    (point.x == o.point.x && point.y == o.point.y && point.z == o.point.z));
        }
    };
    TargetMark target_mark() const { return {target_entity_id, has_ground_target, ground_target}; }
    void update_scripted(Unit& owner, EntityRegistry& registry, lua_State* L,
                         const TargetMark& previous);
    /// A manual weapon's target: what its unit's launch order names, if the
    /// order is for this weapon; else none.
    void take_order_target(const Unit& owner, const EntityRegistry& registry);
    /// Hand the target to this weapon's aim controllers (or take it away),
    /// telling the script OnStartTracking/OnStopTracking(label).
    void update_aim(Unit& owner, EntityRegistry& registry, lua_State* L);
};

/// Parse pipe-separated layer string ("Land|Water|Air") into bitmask.
inline uint8_t parse_layer_caps(const std::string& caps) {
    if (caps == "None" || caps.empty()) return 0x00;
    uint8_t mask = 0;
    size_t start = 0;
    while (start < caps.size()) {
        size_t end = caps.find('|', start);
        if (end == std::string::npos) end = caps.size();
        auto token = caps.substr(start, end - start);
        if (token == "Land")        mask |= 0x01;
        else if (token == "Water")  mask |= 0x02;
        else if (token == "Seabed") mask |= 0x04;
        else if (token == "Sub")    mask |= 0x08;
        else if (token == "Air")    mask |= 0x10;
        start = end + 1;
    }
    return mask;
}

/// Map a layer string ("Land", "Water", etc.) to a single-bit mask.
inline uint8_t layer_to_bit(const std::string& layer) {
    if (layer == "Land")   return 0x01;
    if (layer == "Water")  return 0x02;
    if (layer == "Seabed") return 0x04;
    if (layer == "Sub")    return 0x08;
    if (layer == "Air")    return 0x10;
    return 0x00;
}

} // namespace osc::sim
