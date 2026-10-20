#pragma once

// A frame's cost on the GPU (M223b): timestamps at its command buffer's start
// and end and between its passes, and a pipeline statistics query over it
// where the device has one.
// The results are read when the frame's slot comes round again, after its
// fence, so reading never stalls the GPU.

#include "core/types.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <span>
#include <vector>

namespace osc::renderer {

class GpuFrameQueries {
public:
    /// Where a frame's GPU time goes. Each mark() starts a span of its pass
    /// that runs to the next mark (or the frame's end); a pass marked twice
    /// (two refraction copies, split views) sums. Other: whatever no mark
    /// names (movies, the terrain's sync, the minimap's frame, captures).
    enum class Pass : u8 {
        Other,
        Shadow,
        Normals,
        Reflection,
        Main,
        RefractionCopy,
        Bloom,
        Composite
    };
    static constexpr size_t kPasses = 8;
    static constexpr std::array<const char*, kPasses> kPassNames = {
        "other", "shadow",          "normals", "reflection",
        "main",  "refraction_copy", "bloom",   "composite"};
    using PassTimes = std::array<f64, kPasses>;

    /// `ticks[i + 1] - ticks[i]` (under `valid_mask`, so a wrapped counter
    /// still counts) goes to `passes[i]`'s time.
    static PassTimes split(std::span<const u64> ticks, std::span<const Pass> passes,
                           f64 ns_per_tick, u64 valid_mask);

    /// One frame's figures, as far as the device measures them.
    struct Frame {
        u64 sequence = 0;     ///< which frame (Renderer::frame_sequence), 0 for none yet
        bool timed = false;   ///< the device has timestamps on the graphics queue
        f64 gpu_ms = 0;       ///< its command buffer's start to end, on the GPU
        PassTimes pass_ms{};  ///< gpu_ms by pass (they sum to it)
        bool counted = false; ///< the device has pipeline statistics
        u64 primitives = 0;   ///< primitives the input assembler put out
        u64 vertex_invocations = 0;
        u64 fragment_invocations = 0;
    };

    /// `slots` frames in flight. `statistics`: the device's
    /// pipelineStatisticsQuery feature was enabled.
    void init(VkPhysicalDevice physical, VkDevice device, u32 queue_family, bool statistics,
              u32 slots);
    void destroy(VkDevice device);

    /// After the fence of `slot` is waited on: the results of the frame that
    /// last used it, if one did, become latest().
    void collect(VkDevice device, u32 slot);
    /// At the start of `slot`'s command buffer, before any render pass.
    void begin(VkCommandBuffer cmd, u32 slot, u64 sequence);
    /// Between begin() and end(), outside any render pass: the GPU's work
    /// from here is `pass`'s, in the command buffer begun last (whichever
    /// slot a split view's second view draws from). Only while timing passes:
    /// on a tiler a timestamp between passes can cost the frame a millisecond.
    void mark(VkCommandBuffer cmd, Pass pass);
    void set_timing_passes(bool on) { timing_passes_ = on; }
    /// At its end, after the last render pass.
    void end(VkCommandBuffer cmd, u32 slot);

    /// The latest frame whose results are in: a frame in flight behind.
    const Frame& latest() const { return latest_; }

private:
    static constexpr u32 kMaxMarks = 32; ///< per frame; marks past it fold into the span before
    static constexpr u32 kQueriesPerSlot = kMaxMarks + 2; ///< its start, its marks, its end
    struct Slot {
        u64 sequence = 0;         ///< the frame it holds results for (0: none)
        std::vector<Pass> passes; ///< the span each timestamp after the first starts
    };
    VkQueryPool timestamps_ = VK_NULL_HANDLE;
    VkQueryPool statistics_ = VK_NULL_HANDLE;
    f64 ns_per_tick_ = 0;
    u64 valid_mask_ = 0;
    std::vector<Slot> slots_;
    bool timing_passes_ = false;
    u32 open_ = ~0u; ///< the slot begun and not yet ended (none: ~0u)
    Frame latest_;
};

} // namespace osc::renderer
