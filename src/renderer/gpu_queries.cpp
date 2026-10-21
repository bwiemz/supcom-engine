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

u64 GpuFrameQueries::ticks_between(u64 from, u64 to, u64 valid_mask) {
    const u64 span = ((to & valid_mask) - (from & valid_mask)) & valid_mask;
    return span > (valid_mask >> 1) ? 0 : span;
}

GpuFrameQueries::PassTimes GpuFrameQueries::split(std::span<const u64> ticks,
                                                  std::span<const Pass> passes, f64 ns_per_tick,
                                                  u64 valid_mask) {
    PassTimes ms{};
    for (size_t i = 0; i + 1 < ticks.size() && i < passes.size(); ++i)
        ms[static_cast<size_t>(passes[i])] +=
            static_cast<f64>(ticks_between(ticks[i], ticks[i + 1], valid_mask)) * ns_per_tick /
            1.0e6;
    return ms;
}

u32 GpuFrameQueries::Marks::begin(bool timing) {
    timing_ = timing;
    count_ = 1;
    passes_[0] = Pass::Other;
    return timing ? kQueriesPerSlot : 2;
}

std::optional<u32> GpuFrameQueries::Marks::mark(Pass pass) {
    if (!timing_ || count_ > kMaxMarks) return std::nullopt;
    passes_[count_] = pass;
    return count_++;
}

void GpuFrameQueries::init(VkPhysicalDevice physical, VkDevice device, u32 queue_family,
                           bool statistics, u32 slots) {
    slots_.assign(slots, {});
    latest_ = {};
    ns_per_tick_ = 0;

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
        const u32 written = held.marks.written();
        std::array<u64, kQueriesPerSlot> ticks{};
        if (vkGetQueryPoolResults(device, timestamps_, kQueriesPerSlot * slot, written,
                                  written * sizeof(u64), ticks.data(), sizeof(u64),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            frame.timed = true;
            frame.gpu_ms =
                static_cast<f64>(ticks_between(ticks[0], ticks[written - 1], valid_mask_)) *
                ns_per_tick_ / 1.0e6;
            if (held.marks.timing()) {
                frame.passes_timed = true;
                frame.pass_ms = split(std::span(ticks.data(), written), held.marks.passes(),
                                      ns_per_tick_, valid_mask_);
                for (const Pass p : held.marks.passes())
                    frame.pass_marked[static_cast<size_t>(p)] = true;
            }
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
    const u32 reset = slots_[slot].marks.begin(timing_passes_ && timestamps_);
    if (timestamps_) {
        vkCmdResetQueryPool(cmd, timestamps_, kQueriesPerSlot * slot, reset);
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
    if (open_ >= slots_.size() || slots_[open_].sequence == 0) return;
    if (const std::optional<u32> query = slots_[open_].marks.mark(pass))
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps_,
                            kQueriesPerSlot * open_ + *query);
}

void GpuFrameQueries::end(VkCommandBuffer cmd, u32 slot) {
    open_ = ~0u;
    if (slot >= slots_.size() || slots_[slot].sequence == 0) return;
    if (statistics_) vkCmdEndQuery(cmd, statistics_, slot);
    if (timestamps_)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps_,
                            kQueriesPerSlot * slot + slots_[slot].marks.end());
}

} // namespace osc::renderer
