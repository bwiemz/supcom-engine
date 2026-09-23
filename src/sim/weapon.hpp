#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3

#include <string>

struct lua_State;

namespace osc::map {
class VisibilityGrid;
}

namespace osc::sim {

class EntityRegistry;
class Projectile;
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
    f32 muzzle_velocity = 25;
    bool fire_on_death = false;
    bool manual_fire = false;
    bool counted_projectile = false; // CountedProjectile: fires silo ammo
    bool overcharge = false;         // OverChargeWeapon
    bool beam = false;               // BeamLifetime: a DefaultBeamWeapon
    std::string muzzle_bone_name; // from RackBones[1].MuzzleBones[1]
    f32 firing_randomness = 0;    // angular scatter in radians
    uint8_t fire_target_layer_caps = 0xFF; // bitmask: default = all layers
    f32 max_height_diff = 0;        // ChangeMaxHeightDiff
    f32 firing_tolerance = 0;       // ChangeFiringTolerance
    std::string projectile_bp_id;   // ChangeProjectileBlueprint
    bool target_ground = false;       // SetTargetGround
    bool fire_control = false;        // SetFireControl / IsFireControl
    bool need_compute_bomb_drop = false; // NeedToComputeBombDrop
    f32 bomb_drop_threshold = 25.0f;     // BombDropThreshold (default 25)
    int targeting_priorities_ref = -2; // LUA_NOREF: SetTargetingPriorities Lua table ref
    int weapon_priorities_ref = -2;    // LUA_NOREF: SetWeaponPriorities Lua table ref
    int blueprint_ref = -2;     // LUA_NOREF = Lua registry ref to weapon bp table
    int lua_table_ref = -2;     // LUA_NOREF = Lua ref to weapon Lua table
    bool script_class = false;  // the Lua object is an instance of a weapon class
    i32 weapon_index = 0;       // 0-based index within unit
    u32 owner_entity_id = 0;    // back-pointer to owning unit

    // Runtime state
    u32 target_entity_id = 0;   // 0 = no target
    bool enabled = true;
    u32 fire_clock = 0; // ticks until the fire clock is ready again

    /// Ticks between shots: 1/RateOfFire, rounded to whole ticks as Moho
    /// does (at least one).
    u32 fire_period() const;

    /// Retail's firing cycle drives this weapon: the engine picks targets and
    /// runs the fire clock, and the weapon's script state machine gets
    /// OnGotTarget/OnLostTarget/OnFire and fires its own racks and salvos.
    /// Silo, OverCharge and beam weapons keep the engine's own firing until
    /// their commands (M206) and beam collision exist.
    bool fires_through_script() const {
        return script_class && lua_table_ref >= 0 && !counted_projectile && !overcharge && !beam;
    }

    /// Moho's CanFire: a target, the weapon enabled, the unit free (not
    /// Busy), and a bomber over its drop zone. Aim is always on target
    /// until turrets exist (M200d).
    bool can_fire(const Unit& owner, const EntityRegistry& registry) const;

    /// Per tick: advance the fire clock, pick targets, fire when ready.
    void update(Unit& owner, EntityRegistry& registry, lua_State* L,
                const map::VisibilityGrid* visibility_grid = nullptr);

    /// Fire the weapon at current target. Returns true if fired.
    /// Fire one projectile from `spawn_pos` at `target` (along the owner's
    /// facing when null): the weapon's muzzle velocity and spread, the
    /// projectile blueprint's physics, the weapon's damage. It is registered
    /// and given its script object (OnCreate runs). Returns it.
    Projectile* launch(Unit& owner, const Vector3& spawn_pos, const Entity* target,
                       EntityRegistry& registry, lua_State* L, bool in_water);
    bool try_fire(Unit& owner, EntityRegistry& registry, lua_State* L,
                  const map::VisibilityGrid* visibility_grid = nullptr);

private:
    void update_targeting(Unit& owner, EntityRegistry& registry,
                          const map::VisibilityGrid* visibility_grid);
    void update_scripted(Unit& owner, EntityRegistry& registry, lua_State* L, u32 previous_target);
    /// Call the weapon script's `method(self)`, if it has one. Returns its
    /// first result's truth (true when there is no such method).
    bool call_script(lua_State* L, const char* method) const;
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
