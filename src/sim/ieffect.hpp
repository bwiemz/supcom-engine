#pragma once

#include "core/types.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace osc::sim {

/// How many ticks an emitter with this Lifetime (ticks: its blueprint's, or
/// a LIFETIME param) emits before it ends, or -1 for one that emits on.
/// Moho's CEfxEmitter::ProcessLifetime counts the emitter's age from 0 and
/// ends it once age >= Lifetime, so a fraction rounds up (a muzzle flash's
/// 0.1 emits for one tick), 0 ends it before it emits, and only a negative
/// Lifetime (smoke, a projectile's trail) never ends. Repeattime has no part
/// in it: Moho samples the emission curves at the age modulo Repeattime.
inline f64 emitter_life_ticks(f64 lifetime) {
    return lifetime >= 0 ? std::ceil(lifetime) : -1.0;
}

/// Effect type determines creation semantics and future rendering behavior.
enum class EffectType : u8 {
    EMITTER_AT_ENTITY,     // CreateEmitterAtEntity
    EMITTER_AT_BONE,       // CreateEmitterAtBone
    ATTACHED_EMITTER,      // CreateAttachedEmitter / CreateEmitterOnEntity (follows entity)
    BEAM_EMITTER,          // CreateBeamEmitter (unattached beam visual)
    ATTACHED_BEAM,         // CreateAttachedBeam (fixed-length beam on entity)
    BEAM_ENTITY_TO_ENTITY, // AttachBeamEntityToEntity / CreateBeamEntityToEntity
    LIGHT_PARTICLE,        // CreateLightParticle / CreateLightParticleIntel
    DECAL,                 // CreateDecal
    SPLAT,                 // CreateSplat
    TRAIL_EMITTER,         // CreateTrail (a projectile's or unit's polytrail, follows it)
};

/// Lightweight tracked VFX object. Returned by Create*Emitter/Beam/Decal globals.
/// Uses _c_object lightuserdata pattern for Lua binding.
/// Currently state-tracking only; rendering comes in a later milestone.
class IEffect {
public:
    u32 id() const { return id_; }
    void set_id(u32 i) { id_ = i; }

    EffectType type() const { return type_; }
    void set_type(EffectType t) { type_ = t; }

    const std::string& blueprint_path() const { return blueprint_path_; }
    void set_blueprint_path(const std::string& p) { blueprint_path_ = p; }

    u32 entity_id() const { return entity_id_; }
    void set_entity_id(u32 id) { entity_id_ = id; }

    u32 target_entity_id() const { return target_entity_id_; }
    void set_target_entity_id(u32 id) { target_entity_id_ = id; }

    i32 bone_index() const { return bone_index_; }
    void set_bone_index(i32 b) { bone_index_ = b; }

    i32 target_bone_index() const { return target_bone_index_; }
    void set_target_bone_index(i32 b) { target_bone_index_ = b; }

    i32 army() const { return army_; }
    void set_army(i32 a) { army_ = a; }

    f32 scale() const { return scale_; }
    void set_scale(f32 s) { scale_ = s; }

    f32 offset_x() const { return offset_x_; }
    f32 offset_y() const { return offset_y_; }
    f32 offset_z() const { return offset_z_; }
    void set_offset(f32 x, f32 y, f32 z) { offset_x_ = x; offset_y_ = y; offset_z_ = z; }

    bool destroyed() const { return destroyed_; }
    void mark_destroyed() { destroyed_ = true; }

    int lua_table_ref() const { return lua_table_ref_; }
    void set_lua_table_ref(int r) { lua_table_ref_ = r; }

    /// Runtime parameter overrides (SetEmitterParam).
    void set_param(const std::string& name, f64 value) {
        params_[name] = value;
        if (name == "LIFETIME") lifetime_ = value;
    }
    f64 get_param(const std::string& name) const {
        auto it = params_.find(name);
        return it != params_.end() ? it->second : 0.0;
    }

    /// When an emitter stops of itself (game seconds; negative: it emits
    /// on): its blueprint's Lifetime after it was made, or a script's
    /// LIFETIME param. Its effect then ends, as Moho's emitter does; the
    /// renderer lets the particles already out fade.
    f64 ends_at() const { return ends_at_; }
    void set_ends_at(f64 t) { ends_at_ = t; }
    /// The tick it was made on, which an emitter's lifetime counts from.
    u32 created_tick() const { return created_tick_; }
    void set_created_tick(u32 tick) { created_tick_ = tick; }
    /// Whether it runs a particle emitter blueprint, whose Lifetime ends it.
    /// (A beam made through an emitter creator runs until its entity goes.)
    bool has_emitter_blueprint() const { return has_emitter_blueprint_; }
    void set_has_emitter_blueprint(bool v) { has_emitter_blueprint_ = v; }
    /// End this emitter once it has emitted for `lifetime` ticks (see
    /// emitter_life_ticks); a negative lifetime emits on. The end is a whole
    /// tick, times `seconds_per_tick` as game time is, so it falls exactly
    /// on that tick.
    void end_after(f64 lifetime, f64 seconds_per_tick);

    /// Birth time for lifetime tracking (game seconds).
    f64 birth_time() const { return birth_time_; }
    /// get_param("LIFETIME"), kept at hand: expire_timed asks it of every
    /// timed effect each tick.
    f64 lifetime() const { return lifetime_; }
    void set_birth_time(f64 t) { birth_time_ = t; }

    /// Light particle specific fields.
    f32 light_size() const { return light_size_; }
    void set_light_size(f32 s) { light_size_ = s; }
    f32 light_duration() const { return light_duration_; }
    void set_light_duration(f32 d) { light_duration_ = d; }
    const std::string& glow_texture() const { return glow_texture_; }
    void set_glow_texture(const std::string& t) { glow_texture_ = t; }
    const std::string& ramp_texture() const { return ramp_texture_; }
    void set_ramp_texture(const std::string& t) { ramp_texture_ = t; }

private:
    u32 id_ = 0;
    EffectType type_ = EffectType::EMITTER_AT_ENTITY;
    std::string blueprint_path_;
    u32 entity_id_ = 0;        // parent entity
    u32 target_entity_id_ = 0; // for beam entity-to-entity
    i32 bone_index_ = -1;
    i32 target_bone_index_ = -1;
    i32 army_ = 0;
    f32 scale_ = 1.0f;
    f32 offset_x_ = 0, offset_y_ = 0, offset_z_ = 0;
    bool destroyed_ = false;
    int lua_table_ref_ = -2; // LUA_NOREF
    std::unordered_map<std::string, f64> params_;

    f64 birth_time_ = -1.0; // -1 = no auto-expiry
    f64 lifetime_ = 0.0;    // params_' LIFETIME (0 when unset)
    f64 ends_at_ = -1.0;    // an emitter's end (see ends_at)
    u32 created_tick_ = 0;
    bool has_emitter_blueprint_ = false;

    // Light particle fields
    f32 light_size_ = 0;
    f32 light_duration_ = 0;
    std::string glow_texture_;
    std::string ramp_texture_;
};

inline void IEffect::end_after(f64 lifetime, f64 seconds_per_tick) {
    const f64 ticks = emitter_life_ticks(lifetime);
    ends_at_ = ticks >= 0 ? (static_cast<f64>(created_tick_) + ticks) * seconds_per_tick : -1.0;
}

/// Registry that owns all IEffect instances. Provides creation, lookup, and cleanup.
/// Scripts refer to effects by id (never by pointer): gc() frees destroyed
/// effects while trash bags may still Destroy() their handles.
class IEffectRegistry {
public:
    /// Create a new IEffect with auto-incremented ID.
    IEffect* create() {
        auto fx = std::make_unique<IEffect>();
        fx->set_id(next_id_++);
        auto* ptr = fx.get();
        effects_.push_back(std::move(fx));
        by_id_[ptr->id()] = ptr;
        return ptr;
    }

    /// The live (not destroyed) effect with this id, or nullptr.
    IEffect* find(u32 id) {
        auto it = by_id_.find(id);
        return it != by_id_.end() && !it->second->destroyed() ? it->second : nullptr;
    }

    /// Mark timed effects whose lifetime has expired as destroyed: decals,
    /// splats and lights by their LIFETIME, and emitters that have stopped
    /// emitting.
    void expire_timed(f64 game_time) {
        for (auto& fx : effects_) {
            if (!fx || fx->destroyed()) continue;
            if (fx->ends_at() >= 0 && game_time >= fx->ends_at()) {
                fx->mark_destroyed();
                continue;
            }
            if (fx->birth_time() < 0) continue; // no auto-expiry
            f64 lifetime = fx->lifetime();
            if (lifetime > 0 && (game_time - fx->birth_time()) >= lifetime) {
                fx->mark_destroyed();
            }
        }
    }

    /// Destroy the effects that follow an entity which is gone: its attached
    /// emitters, trails and beams go with it, as in Moho. (One-shot emitters
    /// placed at an entity or bone stay: a death explosion outlives the unit.)
    /// `gone(id)` says whether entity `id` no longer exists.
    template <typename Gone> void destroy_detached(Gone gone) {
        for (auto& fx : effects_) {
            if (!fx || fx->destroyed() || fx->entity_id() == 0) continue;
            const EffectType t = fx->type();
            if (t != EffectType::ATTACHED_EMITTER && t != EffectType::TRAIL_EMITTER &&
                t != EffectType::ATTACHED_BEAM)
                continue;
            if (gone(fx->entity_id())) fx->mark_destroyed();
        }
    }

    /// Remove destroyed effects (call periodically).
    void gc() {
        for (const auto& fx : effects_) {
            if (fx && fx->destroyed()) by_id_.erase(fx->id());
        }
        effects_.erase(
            std::remove_if(effects_.begin(), effects_.end(),
                           [](const std::unique_ptr<IEffect>& fx) {
                               return !fx || fx->destroyed();
                           }),
            effects_.end());
    }

    const std::vector<std::unique_ptr<IEffect>>& all() const { return effects_; }
    size_t count() const { return effects_.size(); }

    /// An emitter blueprint's Lifetime (ticks; none for a path that isn't
    /// an emitter blueprint), from `compute` the first time it is asked.
    template <typename Compute>
    std::optional<f64> blueprint_lifetime(const std::string& path, Compute compute) {
        auto it = lifetime_by_blueprint_.find(path);
        if (it == lifetime_by_blueprint_.end())
            it = lifetime_by_blueprint_.emplace(path, compute()).first;
        return it->second;
    }

private:
    std::vector<std::unique_ptr<IEffect>> effects_;
    std::unordered_map<u32, IEffect*> by_id_; ///< lookup only; never iterated
    std::unordered_map<std::string, std::optional<f64>> lifetime_by_blueprint_; ///< lookup only
    u32 next_id_ = 1;
};

} // namespace osc::sim
