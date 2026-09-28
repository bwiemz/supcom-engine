// The shoreline's waves (M213c), from faf-re's terrain/water/WaveSystem.cpp:
// WaveGenerator::LoadSerializedState, RefreshTextureHandlesAndSchedule,
// RebuildSpatialBounds and Update, and WaveSystem::Update.

#include "renderer/wave_system.hpp"

#include "map/scmap_parser.hpp"
#include "renderer/frustum.hpp"

#include <algorithm>
#include <cmath>

namespace osc::renderer {

namespace {

/// The generators' bounds reach this far up and down (kWaveBoundsHalfHeight).
constexpr f32 kBoundsHalfHeight = 0.1f;
/// The in-view generators are looked for again every fifth tick.
constexpr u32 kLookEvery = 5;
/// A frame longer than this emits nothing.
constexpr f32 kLongestFrame = 200.0f;

} // namespace

f32 WaveSystem::random() {
    u64 z = (random_state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<f32>(z >> 40) * (1.0f / 16777216.0f);
}

void WaveSystem::clear() {
    generators_.clear();
    in_view_.clear();
    blueprints_.clear();
    random_state_ = kSeed;
}

void WaveSystem::load(const std::vector<map::ScmapWaveGenerator>& generators, f64 now) {
    clear();
    generators_.reserve(generators.size());
    for (const map::ScmapWaveGenerator& w : generators) {
        Generator g;
        auto& bp = blueprints_[{w.texture, w.ramp, w.frame_count, w.strip_count}];
        if (!bp) {
            bp = std::make_unique<EmitterBlueprintData>();
            bp->blueprint_id = "wave:" + w.texture;
            bp->texture = w.texture;
            bp->ramp_texture = w.ramp;
            bp->frame_count = w.frame_count;
            bp->strip_count = w.strip_count;
            bp->flat = true;   // TRampAnimateFlat
            bp->blendmode = 0; // BlendMode::Mode0, ALPHABLEND
            bp->local_velocity = false;
        }
        g.bp = bp.get();
        g.position = {w.position[0], w.position[1], w.position[2]};
        g.direction = {w.direction[0], w.direction[1], w.direction[2]};
        g.angle = w.angle;
        std::copy(std::begin(w.lifetime), std::end(w.lifetime), g.lifetime);
        std::copy(std::begin(w.interval), std::end(w.interval), g.interval);
        g.begin_size = w.begin_size;
        g.end_size = w.end_size;
        std::copy(std::begin(w.frame_rate), std::end(w.frame_rate), g.frame_rate);
        g.strip_count = w.strip_count;

        // RefreshTextureHandlesAndSchedule: out of step with the rest
        g.last = now - static_cast<f64>(range(0.0f, g.interval[1]));
        g.interval_now = static_cast<f64>(range(g.interval[0], g.interval[1]));

        // RebuildSpatialBounds: how far a wave may drift in one sampled
        // lifetime, plus half its larger size
        const f32 lifetime = range(g.lifetime[0], g.lifetime[1]);
        const f32 speed = std::sqrt(g.direction.x * g.direction.x + g.direction.y * g.direction.y +
                                    g.direction.z * g.direction.z);
        const f32 r = speed * lifetime + std::max(g.begin_size, g.end_size) * 0.5f;
        g.min = {g.position.x - r, g.position.y - kBoundsHalfHeight, g.position.z - r};
        g.max = {g.position.x + r, g.position.y + kBoundsHalfHeight, g.position.z + r};
        generators_.push_back(g);
    }
}

void WaveSystem::emit(Generator& g, f64 now, std::vector<WaveParticle>& out) {
    if (now - g.last <= g.interval_now) return;
    WaveParticle p;
    p.bp = g.bp;
    p.position = g.position;
    p.velocity = g.direction;
    p.lifetime = range(g.lifetime[0], g.lifetime[1]);
    p.begin_size = g.begin_size;
    p.end_size = g.end_size;
    p.angle = g.angle;
    p.framerate = range(g.frame_rate[0], g.frame_rate[1]);
    // A strip of the whole ones (the count truncated), at 1/count a strip
    // (a count under 1 is one strip, as the particles' draw takes it)
    const f32 strips = std::trunc(g.strip_count);
    p.texture_selection = std::floor(strips * random()) / std::max(g.strip_count, 1.0f);
    out.push_back(p);
    g.interval_now = static_cast<f64>(range(g.interval[0], g.interval[1]));
    g.last = now;
}

void WaveSystem::update(const Frustum& frustum, f32 elapsed_seconds, u32 tick, f64 now,
                        std::vector<WaveParticle>& out) {
    if (generators_.empty() || elapsed_seconds > kLongestFrame) return;
    if (tick % kLookEvery == 0) {
        in_view_.clear();
        for (size_t i = 0; i < generators_.size(); ++i)
            if (frustum.is_box_visible(generators_[i].min, generators_[i].max))
                in_view_.push_back(i);
    }
    for (size_t i : in_view_) emit(generators_[i], now, out);
}

} // namespace osc::renderer
