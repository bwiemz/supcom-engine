#pragma once

#include "core/types.hpp"
#include "renderer/emitter_blueprint.hpp"
#include "sim/entity.hpp"

#include <array>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <vector>

struct lua_State;

namespace osc::map {
class Terrain;
}

namespace osc::sim {
class FrameView;
struct EffectRecord;
}

namespace osc::renderer {

class Camera;
class Frustum;
class ReconView;
struct WaveParticle;

/// One particle's quad for the GPU this frame: its centre, the two axes a
/// corner (±1, ±1) spans, its texture rectangle and its ramp coordinate.
struct ParticleInstance {
    f32 center[3];
    f32 axis_x[3];
    f32 axis_y[3];
    f32 uv[4];   ///< u offset, u span, v offset, v span
    f32 ramp[2]; ///< age / lifetime, ramp selection
    f32 pad = 0;
};

/// FA's particles (M214c), as Moho's CEfxEmitter emits them and particle.fx's
/// WorldVS moves and draws them. Once a sim tick each emitter it can see
/// emits into the world's particles, reading its blueprint's curves at its
/// clock (modulo Repeattime); each particle then moves by itself from its
/// spawn state (velocity, acceleration, drag) and ages through its ramp.
class ParticleSystem {
public:
    /// The player's intel: an EmitIfVisible emitter emits only where the
    /// player's army sees, and a CreateIfVisible one it doesn't see made is
    /// never made (null: everything seen; M215b).
    void set_recon(const ReconView* recon) { recon_ = recon; }
    /// graphics_Fidelity (0 low, 1 medium, 2 high): an emitter whose
    /// blueprint leaves it out is never made, as Moho destroys it on making.
    void set_fidelity(int fidelity) { fidelity_ = fidelity; }

    /// A particle from outside an emitter: a shoreline wave (M213c). It
    /// joins at the next update, born at that frame's render time, as
    /// CWorldParticles stamps a particle when it uploads it.
    void add_wave(const WaveParticle& wave);

    /// Emit what a tick `view` hasn't shown before brings, then place this
    /// frame's particles. `terrain` (may be null) is the water they snap to.
    void update(const sim::FrameView& view, const Camera& camera, const Frustum* frustum,
                EmitterBlueprintCache& blueprints, lua_State* L, const map::Terrain* terrain);

    /// This frame's quads, in draw order.
    const std::vector<ParticleInstance>& instances() const { return instances_; }
    /// Runs of instances that draw alike: a pass (under the water or not),
    /// a blend, and textures (Moho's particle buckets, by SortOrder).
    struct Group {
        bool under_water = false;
        i32 blendmode = 0;
        bool light = false;
        std::string texture, ramp;
        u32 offset = 0, count = 0;
    };
    const std::vector<Group>& groups() const { return groups_; }

    /// A particle drawn this frame (tests read them).
    struct Drawn {
        u32 effect_id = 0;
        sim::Vector3 center, axis_x, axis_y;
        f32 age = 0, lifetime = 0;
        std::array<f32, 4> uv{};
        f32 ramp_v = 0;
        i32 blendmode = 0;
        bool under_water = false;
    };
    const std::vector<Drawn>& drawn() const { return drawn_; }

    /// A live emitter (the render-state dump and tests read them).
    struct EmitterView {
        u32 effect_id = 0;
        std::string blueprint;
        sim::Vector3 position; ///< where its visibility is judged (mPos)
        f32 clock = 0;         ///< its TICKCOUNT
        u32 missed = 0;        ///< ticks it didn't emit, to catch up
        bool seen = false;     ///< its last look found the player's army sees it
    };
    std::vector<EmitterView> emitters() const;

    /// Whether effect `id` is an emitter or a light it draws (the overlay
    /// leaves those to it).
    bool draws_effect(u32 id) const { return emitters_.count(id) > 0 || lights_.count(id) > 0; }
    /// Whether effect `id` is an emitter it never made (CreateIfVisible and
    /// unseen, or left out at this fidelity) and so never draws: the overlay
    /// leaves those be too.
    bool unmade(u32 id) const { return unmade_.count(id) > 0; }

    u32 particle_count() const { return static_cast<u32>(particles_.size()); }

    /// Forget every emitter and particle (a new game).
    void clear();

    static constexpr u32 MAX_PARTICLES = 16384;
    /// Most ticks an emitter catches up (CEfxEmitter::OnTick).
    static constexpr u32 MAX_CATCHUP = 24;

private:
    struct Frame {
        sim::Vector3 position;
        sim::Quaternion rotation;
    };
    /// One emitter effect (CEfxEmitter).
    struct Emitter {
        const EmitterBlueprintData* bp = nullptr;
        /// Its blueprint with a script's overrides (SetEmitterParam and the
        /// curve calls; M214d), when it has any: `bp` points here.
        std::shared_ptr<const EmitterBlueprintData> own;
        u32 overrides_serial = 0;      ///< the effect's, as last taken
        f32 tick_increment = 1.0f;     ///< TICKINCREMENT
        std::optional<f32> tick_count; ///< the TICKCOUNT a script last set
        std::deque<Frame> frames; ///< its last frames, newest last
        sim::Vector3 offset;      ///< its POSITION params (OffsetEmitter)
        f32 scale = 1.0f;         ///< EFFECT_SCALE (ScaleEmitter)
        f32 clock = 0;            ///< TICKCOUNT
        f32 emissions = 0;        ///< mTotalEmissions: the fraction owed
        u32 missed = 0;           ///< mLife
        sim::Vector3 position;    ///< mPos
        std::optional<u32> first_look;
        bool seen = false;
    };
    /// One particle in the world (SWorldParticle).
    struct Particle {
        const EmitterBlueprintData* bp = nullptr;
        std::shared_ptr<const EmitterBlueprintData> own; ///< keeps an overridden `bp`
        u32 effect_id = 0;
        f64 born = 0; ///< on the render clock
        f32 lifetime = 0;
        sim::Vector3 position, velocity, acceleration;
        f32 begin_size = 0, end_size = 0;
        f32 angle = 0, spin = 0; ///< radians, radians a tick
        f32 resistance = 0;
        f32 framerate = 0, texture_selection = 0, ramp_selection = 0;
    };

    void advance(const sim::FrameView& view, const sim::Vector3& eye, const Frustum* frustum,
                 EmitterBlueprintCache& blueprints, lua_State* L, const map::Terrain* terrain);
    /// CanSeeCam: in its LODCutoff, in view, and in the player's LOS by a
    /// look every fifth tick.
    bool can_see(Emitter& e, const sim::FrameView& view, u32 tick, const sim::Vector3& eye,
                 const Frustum* frustum) const;
    /// Its frame `ticks` back, `cursor` of the way into that tick
    /// (InterpolatePosition).
    Frame frame_at(const Emitter& e, u32 ticks, f32 cursor) const;
    /// Take effect `fx`'s runtime overrides over `base` (M214d).
    static void apply_overrides(Emitter& e, const sim::EffectRecord& fx,
                                const EmitterBlueprintData& base);
    /// A light's one particle, born on `tick`; a LightParticleIntel the
    /// player's army doesn't see then is never made.
    void make_light(const sim::EffectRecord& fx, const sim::FrameView& view, u32 tick);
    /// Emit one tick's particles, `ticks` back (Tick).
    void emit(u32 id, Emitter& e, u32 ticks, u32 now_tick, const map::Terrain* terrain);
    /// A uniform [0, 1) draw from the renderer's own stream (splitmix64),
    /// apart from the sim's and seeded alike each run, so frames reproduce.
    f32 random();

    const ReconView* recon_ = nullptr;
    std::unordered_map<u32, Emitter> emitters_;
    std::unordered_set<u32> lights_;
    std::unordered_set<u32> unknown_; ///< effects with no emitter blueprint
    std::unordered_set<u32>
        unmade_; ///< never made: unseen CreateIfVisible, or not at this fidelity
    int fidelity_ = 2;
    std::vector<Particle> particles_;
    std::vector<Particle> added_; ///< waves, born at the next update
    std::optional<u32> last_tick_;
    u64 random_state_ = 0x2545F4914F6CDD1Dull;
    std::vector<ParticleInstance> instances_;
    std::vector<Group> groups_;
    std::vector<Drawn> drawn_;
    // The draw order's work, kept between updates.
    std::vector<const EmitterBlueprintData*> order_blueprints_;
    std::vector<u32> bucket_starts_;
    std::vector<const Particle*> order_;
};

} // namespace osc::renderer
