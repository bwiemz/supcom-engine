#pragma once

#include "core/types.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace osc::sim {

/// Effect type determines creation semantics and future rendering behavior.
enum class EffectType : u8 {
    EMITTER_AT_ENTITY,   // CreateEmitterAtEntity / CreateEmitterOnEntity
    EMITTER_AT_BONE,     // CreateEmitterAtBone
    ATTACHED_EMITTER,    // CreateAttachedEmitter (persistent, follows entity)
    BEAM_EMITTER,        // CreateBeamEmitter (unattached beam visual)
    ATTACHED_BEAM,       // CreateAttachedBeam (fixed-length beam on entity)
    BEAM_ENTITY_TO_ENTITY, // AttachBeamEntityToEntity / CreateBeamEntityToEntity
    LIGHT_PARTICLE,      // CreateLightParticle / CreateLightParticleIntel
    DECAL,               // CreateDecal
    SPLAT,               // CreateSplat
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
    void set_param(const std::string& name, f64 value) { params_[name] = value; }
    f64 get_param(const std::string& name) const {
        auto it = params_.find(name);
        return it != params_.end() ? it->second : 0.0;
    }

    /// Birth time for lifetime tracking (game seconds).
    f64 birth_time() const { return birth_time_; }
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

    // Light particle fields
    f32 light_size_ = 0;
    f32 light_duration_ = 0;
    std::string glow_texture_;
    std::string ramp_texture_;
};

/// Registry that owns all IEffect instances. Provides creation, lookup, and cleanup.
class IEffectRegistry {
public:
    /// Create a new IEffect with auto-incremented ID.
    IEffect* create() {
        auto fx = std::make_unique<IEffect>();
        fx->set_id(next_id_++);
        auto* ptr = fx.get();
        effects_.push_back(std::move(fx));
        return ptr;
    }

    /// Find effect by ID (linear scan — effects are transient, count is small).
    IEffect* find(u32 id) {
        for (auto& fx : effects_) {
            if (fx && fx->id() == id && !fx->destroyed()) return fx.get();
        }
        return nullptr;
    }

    /// Mark timed effects whose lifetime has expired as destroyed.
    void expire_timed(f64 game_time) {
        for (auto& fx : effects_) {
            if (!fx || fx->destroyed()) continue;
            if (fx->birth_time() < 0) continue; // no auto-expiry
            f64 lifetime = fx->get_param("LIFETIME");
            if (lifetime > 0 && (game_time - fx->birth_time()) >= lifetime) {
                fx->mark_destroyed();
            }
        }
    }

    /// Remove destroyed effects (call periodically).
    void gc() {
        effects_.erase(
            std::remove_if(effects_.begin(), effects_.end(),
                           [](const std::unique_ptr<IEffect>& fx) {
                               return !fx || fx->destroyed();
                           }),
            effects_.end());
    }

    const std::vector<std::unique_ptr<IEffect>>& all() const { return effects_; }
    size_t count() const { return effects_.size(); }

private:
    std::vector<std::unique_ptr<IEffect>> effects_;
    u32 next_id_ = 1;
};

} // namespace osc::sim
