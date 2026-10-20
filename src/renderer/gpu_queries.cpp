#include "renderer/gpu_queries.hpp"

#include <array>
#include <vector>

namespace osc::renderer {

namespace {

constexpr VkQueryPipelineStatisticFlags kStatistics =
    VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
    VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
    VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;

} // namespace

GpuFrameQueries::PassTimes GpuFrameQueries::split(std::span<const u64> ticks,
                                                  std::span<const Pass> passes, f64 ns_per_tick,
                                                  u64 valid_mask) {
    PassTimes ms{};
    for (size_t i = 0; i + 1 < ticks.size() && i < passes.size(); ++i) {
        const u64 span = ((ticks[i + 1] & valid_mask) - (ticks[i] & valid_mask)) & valid_mask;
        ms[static_cast<size_t>(passes[i])] += static_cast<f64>(span) * ns_per_tick / 1.0e6;
    }
    return ms;
}

void GpuFrameQueries::init(VkPhysicalDevice physical, VkDevice device, u32 queue_family,
                           bool statistics, u32 slots) {
    slots_.assign(slots, {});
    latest_ = {};

    // Timestamps: the graphics queue must count them (timestampValidBits),
    // and the device says how long a tick is.
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    u32 family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    const u32 valid_bits =
        queue_family < family_count ? families[queue_family].timestampValidBits : 0;
    if (valid_bits > 0 && props.limits.timestampPeriod > 0) {
        ns_per_tick_ = props.limits.timestampPeriod;
        valid_mask_ = valid_bits >= 64 ? ~0ull : (1ull << valid_bits) - 1;
        VkQueryPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        ci.queryCount = kQueriesPerSlot * slots;
        if (vkCreateQueryPool(device, &ci, nullptr, &timestamps_) != VK_SUCCESS)
            timestamps_ = VK_NULL_HANDLE;
    }
    if (statistics) {
        VkQueryPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        ci.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        ci.queryCount = slots;
        ci.pipelineStatistics = kStatistics;
        if (vkCreateQueryPool(device, &ci, nullptr, &statistics_) != VK_SUCCESS)
            statistics_ = VK_NULL_HANDLE;
    }
}

void GpuFrameQueries::destroy(VkDevice device) {
    if (timestamps_) vkDestroyQueryPool(device, timestamps_, nullptr);
    if (statistics_) vkDestroyQueryPool(device, statistics_, nullptr);
    timestamps_ = VK_NULL_HANDLE;
    statistics_ = VK_NULL_HANDLE;
    slots_.clear();
}

void GpuFrameQueries::collect(VkDevice device, u32 slot) {
    if (slot >= slots_.size() || slots_[slot].sequence == 0) return;
    Slot& held = slots_[slot];
    Frame frame;
    frame.sequence = held.sequence;
    held.sequence = 0;
    if (timestamps_) {
        const u32 count = static_cast<u32>(held.passes.size()) + 1;
        std::vector<u64> ticks(count);
        if (vkGetQueryPoolResults(device, timestamps_, kQueriesPerSlot * slot, count,
                                  ticks.size() * sizeof(u64), ticks.data(), sizeof(u64),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const u64 span =
                ((ticks.back() & valid_mask_) - (ticks[0] & valid_mask_)) & valid_mask_;
            frame.timed = true;
            frame.gpu_ms = static_cast<f64>(span) * ns_per_tick_ / 1.0e6;
            frame.pass_ms = split(ticks, held.passes, ns_per_tick_, valid_mask_);
        }
    }
    if (statistics_) {
        std::array<u64, 3> counts{}; // in the order of their bits
        if (vkGetQueryPoolResults(device, statistics_, slot, 1, sizeof(counts), counts.data(),
                                  sizeof(counts), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            frame.counted = true;
            frame.primitives = counts[0];
            frame.vertex_invocations = counts[1];
            frame.fragment_invocations = counts[2];
        }
    }
    latest_ = frame;
}

void GpuFrameQueries::begin(VkCommandBuffer cmd, u32 slot, u64 sequence) {
    if (slot >= slots_.size()) return;
    open_ = slot;
    slots_[slot].passes.assign(1, Pass::Other);
    if (timestamps_) {
        vkCmdResetQueryPool(cmd, timestamps_, kQueriesPerSlot * slot, kQueriesPerSlot);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps_,
                            kQueriesPerSlot * slot);
    }
    if (statistics_) {
        vkCmdResetQueryPool(cmd, statistics_, slot, 1);
        vkCmdBeginQuery(cmd, statistics_, slot, 0);
    }
    slots_[slot].sequence = (timestamps_ || statistics_) ? sequence : 0;
}

void GpuFrameQueries::mark(VkCommandBuffer cmd, Pass pass) {
    const u32 slot = open_;
    if (!timing_passes_ || slot >= slots_.size() || slots_[slot].sequence == 0 || !timestamps_)
        return;
    std::vector<Pass>& passes = slots_[slot].passes;
    if (passes.size() > kMaxMarks) return;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps_,
                        kQueriesPerSlot * slot + static_cast<u32>(passes.size()));
    passes.push_back(pass);
}

void GpuFrameQueries::end(VkCommandBuffer cmd, u32 slot) {
    open_ = ~0u;
    if (slot >= slots_.size() || slots_[slot].sequence == 0) return;
    if (statistics_) vkCmdEndQuery(cmd, statistics_, slot);
    if (timestamps_)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps_,
                            kQueriesPerSlot * slot + static_cast<u32>(slots_[slot].passes.size()));
}

} // namespace osc::renderer
