#include "app/render_bench.hpp"

#include "app/bench.hpp"
#include "core/profiler.hpp"
#include "core/version.hpp"
#include "map/terrain.hpp"
#include "platform/system_info.hpp"
#include "renderer/renderer.hpp"
#include "sim/army_brain.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <numbers>
#include <utility>

namespace osc::app {

namespace {

/// Frames after the last measured one to wait for its GPU figures (they
/// arrive a frame in flight behind) before giving up on them.
constexpr u32 kGpuGrace = 8;

/// The renderer's profiler zones a frame's CPU time is split by (they nest
/// under Render::frame; the rest of it is the recording in between).
constexpr const char* kZones[] = {
    "Render::frame",          "Render::gpu_wait",        "Render::unit_update",
    "Render::overlay_update", "Render::particle_update", "Render::ui_update",
    "Render::runtime_decals", "Render::shadow_pass",     "Render::normals",
    "Render::reflection",     "Render::main_pass",       "Render::submit",
};

nlohmann::json stats_json(const std::vector<f64>& values) {
    const TickStats s = tick_stats(values);
    return {{"frames", s.ticks},  {"mean_ms", s.mean_ms}, {"p50_ms", s.p50_ms},
            {"p90_ms", s.p90_ms}, {"p95_ms", s.p95_ms},   {"p99_ms", s.p99_ms},
            {"max_ms", s.max_ms}};
}

/// Mean and max of one per-frame count.
template <typename T, typename F> nlohmann::json count_json(const std::vector<T>& samples, F&& of) {
    if (samples.empty()) return {{"mean", 0}, {"max", 0}};
    f64 total = 0;
    f64 most = 0;
    for (const T& s : samples) {
        const auto v = static_cast<f64>(of(s));
        total += v;
        most = std::max(most, v);
    }
    return {{"mean", total / static_cast<f64>(samples.size())}, {"max", most}};
}

const char* scene_name(RenderBench::Scene scene) {
    switch (scene) {
    case RenderBench::Scene::Battle: return "battle";
    case RenderBench::Scene::Late: return "late";
    case RenderBench::Scene::Strategic: return "strategic";
    }
    return "?";
}

} // namespace

std::optional<sim::Vector3> busiest_battle(const sim::SimState& sim, f32 side) {
    struct Square {
        u32 units = 0;
        u64 armies = 0; ///< a bit per (non-civilian) army with a unit there
    };
    std::map<std::pair<i32, i32>, Square> squares; // (row, column): ordered, so ties are stable
    sim.entity_registry().for_each_unit([&](const sim::Entity& e) {
        if (e.destroyed()) return;
        const auto& u = static_cast<const sim::Unit&>(e);
        if (u.is_dying()) return;
        const sim::Vector3 p = u.position();
        Square& sq = squares[{static_cast<i32>(std::floor(p.z / side)),
                              static_cast<i32>(std::floor(p.x / side))}];
        ++sq.units;
        const i32 army = u.army();
        const sim::ArmyBrain* brain = army >= 0 ? sim.army_at(static_cast<size_t>(army)) : nullptr;
        if (brain && !brain->is_civilian() && army < 64) sq.armies |= 1ull << army;
    });
    const std::pair<i32, i32>* best = nullptr;
    u32 most = 0;
    for (const auto& [cell, sq] : squares) {
        const bool fight = (sq.armies & (sq.armies - 1)) != 0; // two or more armies
        if (fight && sq.units > most) {
            most = sq.units;
            best = &cell;
        }
    }
    if (!best) return std::nullopt;
    return sim::Vector3{(static_cast<f32>(best->second) + 0.5f) * side, 0.0f,
                        (static_cast<f32>(best->first) + 0.5f) * side};
}

std::optional<RenderBench::Scene> RenderBench::scene_named(const std::string& name) {
    if (name == "battle") return Scene::Battle;
    if (name == "late") return Scene::Late;
    if (name == "strategic") return Scene::Strategic;
    return std::nullopt;
}

RenderBench::RenderBench(std::string report_path, Scene scene, u32 warmup_frames, u32 frames)
    : path_(std::move(report_path)), scene_(scene), warmup_(warmup_frames),
      frames_(std::max<u32>(frames, 1)) {
    samples_.reserve(frames_);
    gpu_.reserve(frames_);
}

void RenderBench::before_frame(renderer::Renderer& renderer, const sim::SimState& sim) {
    renderer::Camera& camera = renderer.camera();
    if (!started_) {
        started_ = true;
        tick_at_start_ = sim.tick_count();
        const map::Terrain* terrain = sim.terrain();
        const f32 width = terrain ? static_cast<f32>(terrain->map_width()) : 512.0f;
        const f32 height = terrain ? static_cast<f32>(terrain->map_height()) : 512.0f;
        const sim::Vector3 centre{width * 0.5f, 0.0f, height * 0.5f};
        if (scene_ == Scene::Strategic) {
            focus_ = centre;
            base_zoom_ = camera.max_zoom();
        } else {
            focus_ = busiest_battle(sim).value_or(centre);
            // Near enough to see the fight's units; the late game's wider,
            // for the hundreds of units and their effects about it.
            base_zoom_ = scene_ == Scene::Battle ? 80.0f : 180.0f;
        }
    }
    // The path: a quarter orbit about the fight, or a zoom from the whole map
    // to half of it, spread over the warm-up and the measured frames.
    const f32 t =
        static_cast<f32>(std::min(seen_, warmup_ + frames_)) / static_cast<f32>(warmup_ + frames_);
    camera.set_target(focus_.x, focus_.z);
    if (scene_ == Scene::Strategic) {
        camera.set_zoom(base_zoom_ * (1.0f - 0.5f * t));
        camera.set_heading(0.0f);
    } else {
        camera.set_zoom(base_zoom_);
        camera.set_heading(t * std::numbers::pi_v<f32> * 0.5f);
    }
}

void RenderBench::after_frame(const renderer::Renderer& renderer, f64 cpu_ms) {
    ++seen_;
    // The GPU's figures for measured frames, as they come in (a frame in
    // flight behind the frame just recorded).
    const renderer::GpuFrameQueries::Frame& gpu = renderer.last_gpu_frame();
    if (first_measured_sequence_ != 0 && gpu.sequence != last_gpu_sequence_ &&
        gpu.sequence >= first_measured_sequence_ &&
        gpu.sequence < first_measured_sequence_ + frames_) {
        gpu_.push_back(gpu);
        last_gpu_sequence_ = gpu.sequence;
    }
    if (seen_ <= warmup_ || samples_.size() >= frames_) return;
    if (samples_.empty()) first_measured_sequence_ = renderer.frame_sequence();
    Sample s;
    s.cpu_ms = cpu_ms;
    s.gpu_wait_ms = renderer.last_gpu_wait_ms();
    s.counts = renderer.last_command_counts();
    s.mesh_instances = renderer.mesh_instance_count();
    s.particles = renderer.particle_system().particle_count();
    s.beams = static_cast<u32>(renderer.beam_renderer().drawn().size());
    s.trail_segments = static_cast<u32>(renderer.trail_renderer().drawn().size());
    s.icons = static_cast<u32>(renderer.strategic_icons().quads().size());
    for (const char* zone : kZones)
        s.zone_ms.push_back(Profiler::instance().frame_zone_us(zone) / 1000.0);
    samples_.push_back(s);
    const renderer::Renderer::VramUsage vram = renderer.vram_usage();
    vram_allocated_peak_ = std::max(vram_allocated_peak_, vram.allocated_bytes);
    vram_peak_ = std::max(vram_peak_, vram.used_bytes);
    vram_budget_ = vram.budget_bytes;
}

bool RenderBench::done() const {
    if (samples_.size() < frames_) return false;
    // Every measured frame's GPU figures are in, or the device gives none,
    // or they're overdue.
    return gpu_.size() >= frames_ || seen_ >= warmup_ + frames_ + kGpuGrace;
}

nlohmann::json RenderBench::zones_json() const {
    nlohmann::json zones = nlohmann::json::object();
    for (size_t z = 0; z < std::size(kZones); ++z) {
        std::vector<f64> ms;
        for (const Sample& s : samples_)
            if (z < s.zone_ms.size()) ms.push_back(s.zone_ms[z]);
        const TickStats st = tick_stats(ms);
        zones[kZones[z]] = {{"mean_ms", st.mean_ms}, {"p95_ms", st.p95_ms}};
    }
    return zones;
}

bool RenderBench::write(const renderer::Renderer& renderer, const sim::SimState& sim) const {
    std::vector<f64> cpu;
    std::vector<f64> wait;
    for (const Sample& s : samples_) {
        cpu.push_back(s.cpu_ms);
        wait.push_back(s.gpu_wait_ms);
    }
    std::vector<f64> gpu_ms;
    for (const auto& g : gpu_)
        if (g.timed) gpu_ms.push_back(g.gpu_ms);
    const bool counted = !gpu_.empty() && gpu_.front().counted;

#ifdef NDEBUG
    constexpr const char* kBuildType = "release";
#else
    constexpr const char* kBuildType = "debug";
#endif
    const auto counts = [&](auto of) { return count_json(samples_, of); };
    const auto pipeline = [&](auto of) { return count_json(gpu_, of); };
    nlohmann::json report = {
        {"version", core::version()},
        {"build", core::build_id()},
        {"build_type", kBuildType},
        {"os", platform::os_description()},
        {"device", renderer.device_name()},
        {"render",
         {{"scene", scene_name(scene_)},
          {"width", renderer.width()},
          {"height", renderer.height()},
          {"warmup_frames", warmup_},
          {"frames", samples_.size()},
          {"focus", {focus_.x, focus_.z}}}},
        {"cpu", stats_json(cpu)},
        {"cpu_zones", zones_json()},
        {"gpu_wait", stats_json(wait)},
        {"gpu", gpu_ms.empty() ? nlohmann::json(nullptr) : stats_json(gpu_ms)},
        {"commands",
         {{"draws", counts([](const Sample& s) { return s.counts.draws; })},
          {"vertices", counts([](const Sample& s) { return s.counts.vertices; })},
          {"instances", counts([](const Sample& s) { return s.counts.instances; })},
          {"pipeline_binds", counts([](const Sample& s) { return s.counts.pipeline_binds; })},
          {"descriptor_set_binds",
           counts([](const Sample& s) { return s.counts.descriptor_set_binds; })},
          {"descriptor_writes", counts([](const Sample& s) { return s.counts.descriptor_writes; })},
          {"push_constants", counts([](const Sample& s) { return s.counts.push_constants; })}}},
        {"pipeline_statistics",
         counted
             ? nlohmann::json{{"primitives", pipeline([](const auto& g) { return g.primitives; })},
                              {"vertex_invocations",
                               pipeline([](const auto& g) { return g.vertex_invocations; })},
                              {"fragment_invocations",
                               pipeline([](const auto& g) { return g.fragment_invocations; })}}
             : nlohmann::json(nullptr)},
        {"scene",
         {{"mesh_instances", counts([](const Sample& s) { return s.mesh_instances; })},
          {"particles", counts([](const Sample& s) { return s.particles; })},
          {"beams", counts([](const Sample& s) { return s.beams; })},
          {"trail_segments", counts([](const Sample& s) { return s.trail_segments; })},
          {"strategic_icons", counts([](const Sample& s) { return s.icons; })}}},
        {"vram_mb",
         {{"allocated_peak", static_cast<f64>(vram_allocated_peak_) / (1024.0 * 1024.0)},
          {"blocks_peak", static_cast<f64>(vram_peak_) / (1024.0 * 1024.0)},
          {"budget", static_cast<f64>(vram_budget_) / (1024.0 * 1024.0)}}},
        {"game",
         {{"start_tick", tick_at_start_},
          {"end_tick", sim.tick_count()},
          {"checksum", fmt::format("{:08x}", sim.compute_sync_checksum())}}},
    };
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << report.dump(2) << '\n';
    return static_cast<bool>(out.flush());
}

} // namespace osc::app
