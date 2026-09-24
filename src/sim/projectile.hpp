#pragma once

#include "sim/entity.hpp"

#include <string>

struct lua_State;

namespace osc::map { class Terrain; }

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
    bool tracking = false;       // TrackTarget(true/false)
    f32 max_zig_zag = 0;         // ChangeMaxZigZag / GetMaxZigZag
    f32 zig_zag_freq = 0;        // ChangeZigZagFrequency / GetZigZagFrequency
    f32 detonate_above_height = 0;  // ChangeDetonateAboveHeight
    f32 detonate_below_height = 0;  // ChangeDetonateBelowHeight
    bool destroy_on_water = false;   // SetDestroyOnWater
    bool stay_upright = false;       // SetStayUpright
    bool velocity_align = false;     // SetVelocityAlign
    Vector3 angular_velocity;        // SetLocalAngularVelocity
    bool collision_enabled = true;   // SetCollision
    bool collide_surface = true;     // SetCollideSurface
    bool stay_underwater = false;    // StayUnderwater
    /// It hit something: it no longer moves or collides, and its script plays
    /// the rest out (a projectile with an ImpactTimeout lingers for it).
    bool impacted = false;
    /// Below the water's surface: crossing it raises OnEnterWater/OnExitWater.
    bool in_water = false;
    /// Risen above its DetonateBelowHeight: it bursts on coming back down.
    bool burst_armed = false;

    /// Per-tick: move, check collision, impact.
    void update(f64 dt, EntityRegistry& registry, lua_State* L,
                const map::Terrain* terrain = nullptr);

    /// What retail's Projectile.OnImpact calls the thing hit: 'Unit',
    /// 'UnitAir', 'UnitUnderwater', 'Prop', 'Shield', 'Projectile',
    /// 'ProjectileUnderwater', or, reaching the ground, 'Terrain', 'Water' or
    /// 'Underwater'.
    const char* impact_type(const Entity* target, const map::Terrain* terrain) const;

private:
    /// `type` overrides what impact_type() would call it ('Air' for a
    /// detonation in flight).
    void on_impact(lua_State* L, Entity* target, EntityRegistry& registry,
                   const map::Terrain* terrain, const char* type = nullptr);
    /// self:method() on its script, if it has one. False once the projectile
    /// is gone (the script may destroy it).
    bool call_script(lua_State* L, EntityRegistry& registry, const char* method);
    /// The engine's own damage, for projectiles no weapon passed DamageData to
    /// (the engine-fired silo and OverCharge shots).
    void deal_engine_damage(lua_State* L, Entity* target, EntityRegistry& registry);
    static constexpr f32 HIT_RADIUS = 1.5f;
};

} // namespace osc::sim
