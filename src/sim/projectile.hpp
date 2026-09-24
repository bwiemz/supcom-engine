#pragma once

#include "sim/entity.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

struct lua_State;

namespace osc::map { class Terrain; }

namespace osc::sim {

class EntityRegistry;

/// What a projectile's blueprint says of it as a target (M206b), shared by
/// every projectile of the blueprint.
struct ProjectileBlueprintInfo {
    /// Its Categories, and ALLPROJECTILES.
    std::unordered_set<std::string> categories;
    /// DesiredShooterCap: at most this many weapons shoot at it at once (0:
    /// no cap).
    u32 desired_shooter_cap = 0;
};

class Projectile : public Entity {
public:
    bool is_projectile() const override { return true; }

    /// What category tests see (EntityCategoryContains, weapons'
    /// restrictions): its blueprint's categories and ALLPROJECTILES.
    const std::unordered_set<std::string>& categories() const;
    u32 desired_shooter_cap() const { return info_ ? info_->desired_shooter_cap : 0; }
    void set_blueprint_info(std::shared_ptr<const ProjectileBlueprintInfo> info) {
        info_ = std::move(info);
    }
    /// Its layer, for weapons' layer caps: Water below the surface, Air above.
    const char* layer() const { return in_water ? "Water" : "Air"; }
    /// Weapons (unit id, weapon index) that have taken it as their target,
    /// for its DesiredShooterCap. A weapon that has since let go, or whose
    /// unit is gone, still appears until a count prunes it.
    std::vector<std::pair<u32, i32>> shooters;

    /// Moho's gravity, in world units per second squared.
    static constexpr f32 GRAVITY = 4.9f;

    Vector3 velocity;
    u32 target_entity_id = 0;
    Vector3 target_position;
    /// target_position is somewhere it was sent (a weapon's aim, a ground
    /// target, where its target was): a tracking shot flies there.
    bool has_target_position = false;
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
    Vector3 scale_velocity;          // SetScaleVelocity: draw scale change per second
    bool collision_enabled = true;   // SetCollision
    bool collide_surface = true;     // SetCollideSurface: the terrain and water
    bool collide_entity = true;      // SetCollideEntity: units, props, shields
    bool stay_underwater = false;    // StayUnderwater
    /// It hit something: it no longer moves or collides, and its script plays
    /// the rest out (a projectile with an ImpactTimeout lingers for it).
    bool impacted = false;
    /// Below the water's surface: crossing it raises OnEnterWater/OnExitWater.
    bool in_water = false;
    /// Risen above its DetonateBelowHeight: it bursts on coming back down.
    bool burst_armed = false;

    /// Entities an OnCollisionCheck turned it away from: it passes them.
    std::vector<u32> passed;

    /// Per-tick: move, check collision, impact.
    void update(f64 dt, EntityRegistry& registry, lua_State* L,
                const map::Terrain* terrain = nullptr);

    /// What its blueprint's Physics leaves to whoever creates it.
    struct BlueprintPhysics {
        std::optional<bool> use_gravity; ///< Moho's default is to fall
        std::optional<f32> lifetime;
    };
    /// Take its blueprint's Physics: speed, acceleration, tracking, where it
    /// ends of itself, what it collides with.
    BlueprintPhysics apply_blueprint_physics(lua_State* L);

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
    /// This tick's path, `from` to where it is now, against what it can hit:
    /// true if it impacted (it may be gone).
    bool collide(const Vector3& from, EntityRegistry& registry, lua_State* L,
                 const map::Terrain* terrain);
    /// Whether its script and `other`'s both let them meet
    /// (OnCollisionCheck, each given the other).
    bool collision_allowed(lua_State* L, EntityRegistry& registry, Entity& other);

    std::shared_ptr<const ProjectileBlueprintInfo> info_;
};

} // namespace osc::sim
