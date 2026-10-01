#include "renderer/gpu_queries.hpp"

#include <array>

namespace osc::renderer {

namespace {

constexpr VkQueryPipelineStatisticFlags kStatistics =
    VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
    VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
    VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;

} // namespace

void GpuFrameQueries::init(VkPhysicalDevice physical, VkDevice device, u32 queue_family,
                           bool statistics, u32 slots) {
    slot_sequence_.assign(slots, 0);
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
        ci.queryCount = 2 * slots;
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
    slot_sequence_.clear();
}

void GpuFrameQueries::collect(VkDevice device, u32 slot) {
    if (slot >= slot_sequence_.size() || slot_sequence_[slot] == 0) return;
    Frame frame;
    frame.sequence = slot_sequence_[slot];
    slot_sequence_[slot] = 0;
    if (timestamps_) {
        std::array<u64, 2> ticks{};
        if (vkGetQueryPoolResults(device, timestamps_, 2 * slot, 2, sizeof(ticks), ticks.data(),
                                  sizeof(u64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const u64 span = ((ticks[1] & valid_mask_) - (ticks[0] & valid_mask_)) & valid_mask_;
            frame.timed = true;
            frame.gpu_ms = static_cast<f64>(span) * ns_per_tick_ / 1.0e6;
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
    if (slot >= slot_sequence_.size()) return;
    if (timestamps_) {
        vkCmdResetQueryPool(cmd, timestamps_, 2 * slot, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps_, 2 * slot);
    }
    if (statistics_) {
        vkCmdResetQueryPool(cmd, statistics_, slot, 1);
        vkCmdBeginQuery(cmd, statistics_, slot, 0);
    }
    slot_sequence_[slot] = (timestamps_ || statistics_) ? sequence : 0;
}

void GpuFrameQueries::end(VkCommandBuffer cmd, u32 slot) {
    if (slot >= slot_sequence_.size() || slot_sequence_[slot] == 0) return;
    if (statistics_) vkCmdEndQuery(cmd, statistics_, slot);
    if (timestamps_)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps_, 2 * slot + 1);
}

} // namespace osc::renderer
