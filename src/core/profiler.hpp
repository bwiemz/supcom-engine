#pragma once

#include "core/types.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace osc {

/// Per-zone accumulated statistics over a sliding window.
struct ProfileZoneStats {
    const char* name = "";
    f64 last_us = 0;        // most recent sample (microseconds)
    f64 avg_us = 0;         // rolling average
    f64 min_us = 1e12;      // min over window
    f64 max_us = 0;         // max over window
    u32 call_count = 0;     // calls this frame
    u32 depth = 0;          // nesting depth (for display indentation)
};

/// Lightweight performance profiler with scoped timers and per-zone stats.
///
/// Usage:
///   Profiler::instance().begin_frame();
///   { PROFILE_ZONE("Sim"); ... }
///   { PROFILE_ZONE("Render"); ... }
///   Profiler::instance().end_frame();
///
/// Stats are accumulated in a ring buffer for rolling averages.
class Profiler {
public:
    static constexpr u32 HISTORY_SIZE = 120;  // ~2 seconds at 60fps
    static constexpr u32 MAX_ZONES = 64;

    static Profiler& instance() {
        static Profiler s_instance;
        return s_instance;
    }

    void set_enabled(bool e) { enabled_ = e; }
    bool enabled() const { return enabled_; }

    /// Call at the start of each frame to reset per-frame accumulators.
    void begin_frame() {
        if (!enabled_) return;
        frame_zone_count_ = 0;
        depth_ = 0;
    }

    /// Call at the end of each frame to commit samples to the ring buffer.
    void end_frame() {
        if (!enabled_) return;
        frame_count_++;

        // Compute frame total
        auto frame_end = clock::now();
        if (frame_start_valid_) {
            f64 frame_us = to_us(frame_end - frame_start_);
            frame_time_history_[history_index_] = frame_us;
        }
        frame_start_ = frame_end;
        frame_start_valid_ = true;

        // Commit zone samples
        for (u32 i = 0; i < frame_zone_count_; ++i) {
            auto& sample = frame_zones_[i];
            // Find or create persistent zone
            i32 idx = find_or_create_zone(sample.name);
            if (idx < 0) continue;

            auto& zone = zones_[idx];
            zone.depth = sample.depth;
            zone.last_us = sample.elapsed_us;
            zone.call_count = sample.call_count;

            // Update history ring buffer
            auto& hist = zone_history_[idx];
            hist[history_index_] = sample.elapsed_us;

            // Recompute rolling stats
            f64 sum = 0, mn = 1e12, mx = 0;
            u32 valid_count = 0;
            u32 window = std::min(frame_count_, HISTORY_SIZE);
            for (u32 j = 0; j < window; ++j) {
                u32 hi = (history_index_ + HISTORY_SIZE - j) % HISTORY_SIZE;
                f64 v = hist[hi];
                if (v >= 0) {
                    sum += v;
                    mn = std::min(mn, v);
                    mx = std::max(mx, v);
                    valid_count++;
                }
            }
            zone.avg_us = valid_count > 0 ? sum / valid_count : 0;
            zone.min_us = valid_count > 0 ? mn : 0;
            zone.max_us = mx;
        }

        history_index_ = (history_index_ + 1) % HISTORY_SIZE;
    }

    /// Start timing a named zone (typically called via PROFILE_ZONE macro).
    void push_zone(const char* name) {
        if (!enabled_) return;
        if (stack_depth_ >= MAX_STACK) return;

        stack_[stack_depth_] = {name, clock::now(), depth_};
        stack_depth_++;
        depth_++;
    }

    /// End timing the current zone.
    void pop_zone() {
        if (!enabled_ || stack_depth_ == 0) return;

        stack_depth_--;
        auto& entry = stack_[stack_depth_];
        f64 elapsed = to_us(clock::now() - entry.start);
        depth_ = entry.depth;

        // Accumulate into frame zones
        i32 fz = find_frame_zone(entry.name);
        if (fz >= 0) {
            frame_zones_[fz].elapsed_us += elapsed;
            frame_zones_[fz].call_count++;
        } else if (frame_zone_count_ < MAX_ZONES) {
            auto& z = frame_zones_[frame_zone_count_++];
            z.name = entry.name;
            z.elapsed_us = elapsed;
            z.call_count = 1;
            z.depth = entry.depth;
        }
    }

    /// Get all active zone stats (for overlay display or log output).
    const ProfileZoneStats* zone_stats() const { return zones_; }
    u32 zone_count() const { return persistent_zone_count_; }

    /// Get frame time history (for sparkline graph).
    const f64* frame_time_history() const { return frame_time_history_; }
    u32 history_index() const { return history_index_; }
    u32 frame_count() const { return frame_count_; }

    /// Get average frame time in microseconds.
    f64 avg_frame_time_us() const {
        u32 window = std::min(frame_count_, HISTORY_SIZE);
        if (window == 0) return 0;
        f64 sum = 0;
        for (u32 i = 0; i < window; ++i) {
            u32 hi = (history_index_ + HISTORY_SIZE - 1 - i) % HISTORY_SIZE;
            sum += frame_time_history_[hi];
        }
        return sum / window;
    }

    /// Print a summary report to spdlog.
    void log_summary() const {
        if (persistent_zone_count_ == 0) return;

        f64 avg_frame = avg_frame_time_us();
        spdlog::info("=== Performance Profile Summary ===");
        spdlog::info("  Avg frame: {:.1f} us ({:.1f} FPS)",
                     avg_frame, avg_frame > 0 ? 1e6 / avg_frame : 0);
        spdlog::info("  {:<30s} {:>8s} {:>8s} {:>8s} {:>8s}",
                     "Zone", "Last", "Avg", "Min", "Max");
        spdlog::info("  {:-<30s} {:-<8s} {:-<8s} {:-<8s} {:-<8s}",
                     "", "", "", "", "");

        for (u32 i = 0; i < persistent_zone_count_; ++i) {
            auto& z = zones_[i];
            std::string indent(z.depth * 2, ' ');
            spdlog::info("  {:<30s} {:>7.1f}us {:>7.1f}us {:>7.1f}us {:>7.1f}us",
                         indent + z.name, z.last_us, z.avg_us, z.min_us, z.max_us);
        }
        spdlog::info("===================================");
    }

private:
    using clock = std::chrono::high_resolution_clock;
    using time_point = clock::time_point;

    static f64 to_us(std::chrono::high_resolution_clock::duration d) {
        return std::chrono::duration<f64, std::micro>(d).count();
    }

    struct StackEntry {
        const char* name;
        time_point start;
        u32 depth;
    };

    struct FrameZone {
        const char* name = "";
        f64 elapsed_us = 0;
        u32 call_count = 0;
        u32 depth = 0;
    };

    static constexpr u32 MAX_STACK = 16;

    bool enabled_ = false;
    u32 depth_ = 0;
    u32 stack_depth_ = 0;
    StackEntry stack_[MAX_STACK] = {};

    // Per-frame accumulation (reset each begin_frame)
    FrameZone frame_zones_[MAX_ZONES] = {};
    u32 frame_zone_count_ = 0;

    // Persistent zone stats
    ProfileZoneStats zones_[MAX_ZONES] = {};
    u32 persistent_zone_count_ = 0;

    // History ring buffers
    std::array<f64, HISTORY_SIZE> zone_history_[MAX_ZONES] = {};
    f64 frame_time_history_[HISTORY_SIZE] = {};
    u32 history_index_ = 0;
    u32 frame_count_ = 0;

    time_point frame_start_;
    bool frame_start_valid_ = false;

    i32 find_frame_zone(const char* name) const {
        for (u32 i = 0; i < frame_zone_count_; ++i) {
            if (std::strcmp(frame_zones_[i].name, name) == 0)
                return static_cast<i32>(i);
        }
        return -1;
    }

    i32 find_or_create_zone(const char* name) {
        for (u32 i = 0; i < persistent_zone_count_; ++i) {
            if (std::strcmp(zones_[i].name, name) == 0)
                return static_cast<i32>(i);
        }
        if (persistent_zone_count_ < MAX_ZONES) {
            zones_[persistent_zone_count_].name = name;
            // Initialize history to -1 (invalid)
            zone_history_[persistent_zone_count_].fill(-1.0);
            return static_cast<i32>(persistent_zone_count_++);
        }
        return -1;
    }
};

/// RAII scoped timer that pushes/pops a profiler zone.
struct ProfileScope {
    ProfileScope(const char* name) { Profiler::instance().push_zone(name); }
    ~ProfileScope() { Profiler::instance().pop_zone(); }
};

} // namespace osc

/// Convenience macro for scoped profiling. Uses a static string literal.
#define PROFILE_CONCAT_(a, b) a##b
#define PROFILE_CONCAT(a, b) PROFILE_CONCAT_(a, b)
#define PROFILE_ZONE(name) ::osc::ProfileScope PROFILE_CONCAT(_profile_scope_, __LINE__)(name)
