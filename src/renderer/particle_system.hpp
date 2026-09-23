#pragma once

#include "core/types.hpp"
#include "renderer/emitter_blueprint.hpp"
#include "renderer/frustum.hpp"

#include <string>
#include <vector>

namespace osc::sim {
class FrameView;
} // namespace osc::sim

namespace osc::renderer {

/// A single live particle in the CPU simulation.
struct Particle {
    f32 pos_x = 0, pos_y = 0, pos_z = 0;
    f32 vel_x = 0, vel_y = 0, vel_z = 0;
    f32 accel_x = 0, accel_y = 0, accel_z = 0;
    f32 size_start = 1.0f;
    f32 size_end = 1.0f;
    f32 rotation = 0;       // radians
    f32 rotation_rate = 0;  // radians/sec
    f32 lifetime = 1.0f;    // total lifetime
    f32 age = 0;            // seconds alive
    f32 frame_rate = 0;     // texture animation frame rate
    u32 texture_frame = 0;  // starting texture frame
    u32 ramp_frame = 0;     // ramp texture frame
};

/// Per-emitter runtime state: tracks one IEffect's emitter instance.
struct EmitterState {
    const EmitterBlueprintData* blueprint = nullptr;
    u32 effect_id = 0;          // IEffect::id()
    f32 emitter_time = 0;       // time along emitter lifetime
    f32 emit_accumulator = 0;   // fractional particles to emit
    f32 origin_x = 0, origin_y = 0, origin_z = 0; // world position
    bool active = true;

    std::vector<Particle> particles;
};

/// GPU-ready particle instance data (one per live particle).
/// Matches vertex shader input for instanced billboard rendering.
struct ParticleInstance {
    f32 pos_x, pos_y, pos_z;   // world position
    f32 size;                   // current billboard half-extent
    f32 rotation;               // billboard rotation (radians)
    f32 alpha;                  // opacity (0..1), from size curve or age fade
    f32 uv_x, uv_y;            // texture frame offset
    f32 uv_w, uv_h;            // texture frame size
    f32 r, g, b;                // tint color (1,1,1 default)
    f32 ramp_u;                 // life fraction: where the ramp texture is read
};

/// CPU particle simulation. Manages emitter state and particle physics.
/// Each frame, call update() then read instances() for GPU upload.
class ParticleSystem {
public:
    /// Live emitters (the render-state dump reads their origins).
    const std::vector<EmitterState>& emitters() const { return emitters_; }

    /// Sync emitters with the world's effects — create new emitters,
    /// retire those whose effect is gone, place attached ones where `view`
    /// draws their entity.
    void sync_effects(const sim::FrameView& view,
                      EmitterBlueprintCache& bp_cache,
                      struct lua_State* L);

    /// Advance simulation by `dt_seconds`: emit new particles, step physics,
    /// kill expired.
    void update(f32 dt_seconds);

    /// Build GPU instance buffer data from live particles.
    /// Call after update(). Returns the instance array for upload.
    const std::vector<ParticleInstance>& build_instances(
        f32 cam_x, f32 cam_y, f32 cam_z,
        const Frustum* frustum = nullptr);

    /// The instances of build_instances(), in runs that share a texture and
    /// blend: each emitter's particles use its blueprint's `Texture` and
    /// `Blendmode` (alpha-blended runs first, then additive).
    struct TextureGroup {
        std::string texture;   // VFS path; empty for an emitter that names none
        std::string ramp;      // its RampTexture: colour over a particle's life
        bool additive = false; // FA Blendmode 3
        u32 offset = 0;
        u32 count = 0;
    };
    const std::vector<TextureGroup>& texture_groups() const { return groups_; }

    u32 emitter_count() const { return static_cast<u32>(emitters_.size()); }
    u32 particle_count() const;

    /// Remove all emitters and particles (for scene teardown).
    void clear() {
        emitters_.clear();
        instances_.clear();
        groups_.clear();
    }

    static constexpr u32 MAX_PARTICLES = 16384;
    /// Sim ticks per second: emitter blueprints' unit of time.
    static constexpr f32 kTicksPerSecond = 10.0f;

private:
    void emit_particles(EmitterState& es, f32 dt, u32& running_total);
    void step_particles(EmitterState& es, f32 dt);

    std::vector<EmitterState> emitters_;
    std::vector<ParticleInstance> instances_;
    std::vector<TextureGroup> groups_;
};

} // namespace osc::renderer
