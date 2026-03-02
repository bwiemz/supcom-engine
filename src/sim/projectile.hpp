#pragma once

#include "sim/entity.hpp"

#include <string>

struct lua_State;

namespace osc::sim {

class EntityRegistry;

class Projectile : public Entity {
public:
    bool is_projectile() const override { return true; }

    Vector3 velocity;
    u32 target_entity_id = 0;
    Vector3 target_position;
    u32 launcher_id = 0;
    f32 damage_amount = 0;
    f32 damage_radius = 0;
    std::string damage_type = "Normal";
    f32 lifetime = 10.0f;

    // Physics (set by Lua via SetMaxSpeed/SetAcceleration/etc.)
    f32 max_speed = 0;           // 0 = unlimited
    f32 acceleration = 0;        // linear accel per second
    f32 ballistic_accel = 0;     // vertical gravity (negative = down)
    f32 turn_rate = 0;           // degrees/sec (store-only for now)

    /// Per-tick: move, check collision, impact.
    void update(f64 dt, EntityRegistry& registry, lua_State* L);

private:
    void on_impact(lua_State* L, Entity* target, EntityRegistry& registry);
    static constexpr f32 HIT_RADIUS = 1.5f;
};

} // namespace osc::sim
