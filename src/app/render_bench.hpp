#pragma once

// --render-bench <report.json> (M223b): a pinned visual workload's frames,
// timed and counted, for tools/bench.py to compare with a baseline. A scene
// is a saved game (--load) and a camera path over it: the game plays on, and
// after a warm-up (textures loading, emitters filling) each frame is
// recorded. Measuring reads clocks and counters only.

#include "core/types.hpp"
#include "renderer/gpu_queries.hpp"
#include "renderer/vk_cmd.hpp"
#include "sim/entity.hpp"

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <vector>

namespace osc::renderer {
class Renderer;
}
namespace osc::sim {
class SimState;
}

namespace osc::app {

/// Where a battle is: of the side x side squares (world units, from the
/// origin) holding live units of two or more armies (civilians don't count
/// as one), the one with the most live units, the lowest row then column on
/// a tie; nullopt when there is none. The same game picks the same square.
std::optional<sim::Vector3> busiest_battle(const sim::SimState& sim, f32 side = 64.0f);

class RenderBench {
public:
    enum class Scene {
        Battle,   ///< the busiest fight, near: a slow orbit
        Late,     ///< the same rule, wider, for a late game's hundreds of units
        Strategic ///< the whole map, zooming in to half of it
    };
    /// "battle", "late" or "strategic"; nullopt for another name.
    static std::optional<Scene> scene_named(const std::string& name);

    RenderBench(std::string report_path, Scene scene, u32 warmup_frames, u32 frames);

    /// Before a frame renders: the scene's view, the first time the game is
    /// in (its focus chosen then), then the next step of its path.
    void before_frame(renderer::Renderer& renderer, const sim::SimState& sim);
    /// After it: `cpu_ms`, render()'s own time, and what the renderer counted.
    void after_frame(const renderer::Renderer& renderer, f64 cpu_ms);
    /// Every measured frame recorded, and every GPU figure for them in.
    bool done() const;
    /// A frame drawn while the game isn't in yet (the save loading). After
    /// a minute of them it gives up: a save that never loads would
    /// otherwise hold the run open forever.
    void waiting() { ++waited_; }
    bool gave_up() const { return !started_ && waited_ > kMaxWait; }
    /// The report: per-frame figures summarised, the scene, the device.
    bool write(const renderer::Renderer& renderer, const sim::SimState& sim) const;

    const std::string& report_path() const { return path_; }

private:
    nlohmann::json zones_json() const;
    struct Sample {
        f64 cpu_ms = 0;
        f64 gpu_wait_ms = 0;
        renderer::CommandCounts counts;
        u32 mesh_instances = 0;
        u32 particles = 0;
        u32 beams = 0;
        u32 trail_segments = 0;
        u32 icons = 0;
        std::vector<f64> zone_ms; ///< kZones' times this frame (the profiler's)
    };
    std::string path_;
    Scene scene_;
    u32 warmup_;
    u32 frames_;
    static constexpr u32 kMaxWait = 3600; ///< a minute of frames on the fixed clock
    u32 seen_ = 0;                        ///< frames since the scene began, warm-up included
    u32 waited_ = 0;                      ///< frames before it, the save loading
    bool started_ = false;
    sim::Vector3 focus_{};
    f32 base_zoom_ = 0;
    u64 first_measured_sequence_ = 0; ///< the renderer's frame_sequence of the first
    std::vector<Sample> samples_;
    std::vector<renderer::GpuFrameQueries::Frame> gpu_;
    u64 last_gpu_sequence_ = 0;
    u64 vram_allocated_peak_ = 0;
    u64 vram_peak_ = 0;
    u64 vram_budget_ = 0;
    u32 tick_at_start_ = 0;
};

} // namespace osc::app
