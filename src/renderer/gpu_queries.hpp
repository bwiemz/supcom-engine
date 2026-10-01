#pragma once

// A frame's cost on the GPU (M223b): timestamps at its command buffer's start
// and end, and a pipeline statistics query over it where the device has one.
// The results are read when the frame's slot comes round again, after its
// fence, so reading never stalls the GPU.

#include "core/types.hpp"

#include <vulkan/vulkan.h>

#include <vector>

namespace osc::renderer {

class GpuFrameQueries {
public:
    /// One frame's figures, as far as the device measures them.
    struct Frame {
        u64 sequence = 0;     ///< which frame (Renderer::frame_sequence), 0 for none yet
        bool timed = false;   ///< the device has timestamps on the graphics queue
        f64 gpu_ms = 0;       ///< its command buffer's start to end, on the GPU
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
    /// At its end, after the last render pass.
    void end(VkCommandBuffer cmd, u32 slot);

    /// The latest frame whose results are in: a frame in flight behind.
    const Frame& latest() const { return latest_; }

private:
    VkQueryPool timestamps_ = VK_NULL_HANDLE;
    VkQueryPool statistics_ = VK_NULL_HANDLE;
    f64 ns_per_tick_ = 0;
    u64 valid_mask_ = 0;
    std::vector<u64> slot_sequence_; ///< the frame each slot holds results for (0: none)
    Frame latest_;
};

} // namespace osc::renderer
