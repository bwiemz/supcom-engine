#pragma once

#include "core/types.hpp"
#include "renderer/emitter_blueprint.hpp"
#include "sim/entity.hpp"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace osc::map {
struct ScmapWaveGenerator;
}

namespace osc::renderer {

class Frustum;

/// A wave's particle as its generator makes it (WaveGenerator::Update's
/// SWorldParticle, a TRampAnimateFlat one): flat on the water, turned by
/// the generator's angle, drifting by its direction a tick, growing from
/// its begin size to its end size and aging through its ramp.
struct WaveParticle {
    const EmitterBlueprintData* bp = nullptr; ///< its textures, frames and blend
    sim::Vector3 position;
    sim::Vector3 velocity; ///< a tick
    f32 lifetime = 0.0f;   ///< ticks
    f32 begin_size = 0.0f;
    f32 end_size = 0.0f;
    f32 angle = 0.0f; ///< radians
    f32 framerate = 0.0f;
    f32 texture_selection = 0.0f; ///< its strip's top, 0..1
};

/// The shoreline's waves (M213c), as Moho's WaveSystem runs them: every
/// generator the camera sees emits a wave particle each time its interval
/// (a random draw in its range, on the system clock) passes. Which
/// generators it sees is looked at again every fifth tick.
class WaveSystem {
public:
    /// The map's generators (WaveSystem::Load). Each starts out of step: its
    /// last emission a random part of its longest interval before `now`, its
    /// interval a draw in its range (RefreshTextureHandlesAndSchedule).
    void load(const std::vector<map::ScmapWaveGenerator>& generators, f64 now);
    void clear();

    /// WaveSystem::Update, once a frame: nothing without generators or
    /// after a frame of over 200 seconds; on every fifth `tick`, the
    /// generators whose bounds meet `frustum` become the ones that emit;
    /// each of those emits into `out` once its interval has passed at `now`
    /// (seconds, the system clock).
    void update(const Frustum& frustum, f32 elapsed_seconds, u32 tick, f64 now,
                std::vector<WaveParticle>& out);

    size_t generator_count() const { return generators_.size(); }
    /// How many generators emit: those in view at the last look.
    size_t in_view_count() const { return in_view_.size(); }
    /// Generator `i`'s bounds (RebuildSpatialBounds): its position, a
    /// drift's reach plus half its larger size across, 0.1 up and down.
    std::array<f32, 3> bounds_min(size_t i) const { return generators_[i].min; }
    std::array<f32, 3> bounds_max(size_t i) const { return generators_[i].max; }

private:
    struct Generator {
        const EmitterBlueprintData* bp = nullptr;
        sim::Vector3 position;
        sim::Vector3 direction;
        f32 angle = 0.0f;
        f32 lifetime[2] = {0.0f, 0.0f};
        f32 interval[2] = {0.0f, 0.0f};
        f32 begin_size = 0.0f;
        f32 end_size = 0.0f;
        f32 frame_rate[2] = {1.0f, 0.0f};
        f32 strip_count = 1.0f;
        std::array<f32, 3> min{};
        std::array<f32, 3> max{};
        f64 last = 0.0;         ///< mCurrentTime: its last emission
        f64 interval_now = 0.0; ///< mUpdateInterval: the one it waits out
    };

    /// WaveGenerator::Update at `now`.
    void emit(Generator& g, f64 now, std::vector<WaveParticle>& out);
    /// A uniform [0, 1) draw from the waves' own stream (splitmix64),
    /// seeded alike each map, so frames reproduce.
    f32 random();
    /// MathGlobalRandomRange: lo + (hi - lo) * a draw.
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * random(); }

    std::vector<Generator> generators_;
    std::vector<size_t> in_view_;
    /// The waves' draw parameters, one per texture, ramp, frame count and
    /// strip count: flat, alpha-blended (blend mode 0).
    std::map<std::tuple<std::string, std::string, f32, f32>, std::unique_ptr<EmitterBlueprintData>>
        blueprints_;
    u64 random_state_ = kSeed;
    static constexpr u64 kSeed = 0x5A17E5EA5A17E5EAull;
};

} // namespace osc::renderer
