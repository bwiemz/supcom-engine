#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3

#include <string>

struct lua_State;

namespace osc::sim {

class EntityRegistry;
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
    std::string muzzle_bone_name; // from RackBones[1].MuzzleBones[1]
    f32 firing_randomness = 0;    // angular scatter in radians
    uint8_t fire_target_layer_caps = 0xFF; // bitmask: default = all layers
    int blueprint_ref = -2;     // LUA_NOREF = Lua registry ref to weapon bp table
    int lua_table_ref = -2;     // LUA_NOREF = Lua ref to weapon Lua table
    i32 weapon_index = 0;       // 0-based index within unit

    // Runtime state
    u32 target_entity_id = 0;   // 0 = no target
    bool enabled = true;
    f32 fire_cooldown = 0;      // seconds until can fire again

    /// Per-tick: scan for targets, fire if ready.
    void update(f64 dt, Unit& owner, EntityRegistry& registry,
                lua_State* L);

private:
    void update_targeting(Unit& owner, EntityRegistry& registry);
    bool try_fire(Unit& owner, EntityRegistry& registry, lua_State* L);
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
