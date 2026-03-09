#pragma once

#include "core/types.hpp"
#include "renderer/emitter_blueprint.hpp"

#include <vector>

namespace osc::sim {
class IEffect;
class SimState;
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
};

/// CPU particle simulation. Manages emitter state and particle physics.
/// Each frame, call update() then read instances() for GPU upload.
class ParticleSystem {
public:
    /// Sync emitter list with IEffectRegistry — create new emitters,
    /// remove destroyed ones, update positions from entities.
    void sync_effects(const sim::SimState& sim,
                      EmitterBlueprintCache& bp_cache,
                      struct lua_State* L);

    /// Advance simulation: emit new particles, step physics, kill expired.
    void update(f32 dt);

    /// Build GPU instance buffer data from live particles.
    /// Call after update(). Returns the instance array for upload.
    const std::vector<ParticleInstance>& build_instances(
        f32 cam_x, f32 cam_y, f32 cam_z);

    u32 emitter_count() const { return static_cast<u32>(emitters_.size()); }
    u32 particle_count() const;

    static constexpr u32 MAX_PARTICLES = 16384;

private:
    void emit_particles(EmitterState& es, f32 dt, u32& running_total);
    void step_particles(EmitterState& es, f32 dt);

    std::vector<EmitterState> emitters_;
    std::vector<ParticleInstance> instances_;
};

} // namespace osc::renderer
