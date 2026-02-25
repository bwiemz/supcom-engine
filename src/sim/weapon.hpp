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

} // namespace osc::sim
