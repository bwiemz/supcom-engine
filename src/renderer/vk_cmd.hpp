#pragma once

// The renderer's draw and state calls, counted (M223b): what a frame asks of
// the GPU, for the render benchmark. Each forwards to the Vulkan call of its
// name, so the renderers call these in place of vkCmd* and the counts cost a
// few adds.

#include "core/types.hpp"

#include <vulkan/vulkan.h>

namespace osc::renderer {

/// What the command buffers recorded since the counts were last taken.
struct CommandCounts {
    u32 draws = 0;                ///< vkCmdDraw and vkCmdDrawIndexed calls
    u64 vertices = 0;             ///< their vertex (or index) counts × instances
    u64 instances = 0;            ///< their instance counts
    u32 pipeline_binds = 0;       ///< vkCmdBindPipeline calls
    u32 descriptor_set_binds = 0; ///< descriptor sets bound (a call binding two counts two)
    u32 push_constants = 0;       ///< vkCmdPushConstants calls
    u32 descriptor_writes = 0;    ///< VkWriteDescriptorSet entries (vkUpdateDescriptorSets)
};

/// The counts so far. The renderer records on one thread; the benchmark
/// takes them (take_command_counts) once a frame.
CommandCounts& command_counts();

/// The counts so far, starting afresh.
CommandCounts take_command_counts();

namespace vkc {

inline void draw(VkCommandBuffer cmd, u32 vertex_count, u32 instance_count, u32 first_vertex,
                 u32 first_instance) {
    CommandCounts& c = command_counts();
    ++c.draws;
    c.vertices += static_cast<u64>(vertex_count) * instance_count;
    c.instances += instance_count;
    vkCmdDraw(cmd, vertex_count, instance_count, first_vertex, first_instance);
}

inline void draw_indexed(VkCommandBuffer cmd, u32 index_count, u32 instance_count, u32 first_index,
                         i32 vertex_offset, u32 first_instance) {
    CommandCounts& c = command_counts();
    ++c.draws;
    c.vertices += static_cast<u64>(index_count) * instance_count;
    c.instances += instance_count;
    vkCmdDrawIndexed(cmd, index_count, instance_count, first_index, vertex_offset, first_instance);
}

inline void bind_pipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline) {
    ++command_counts().pipeline_binds;
    vkCmdBindPipeline(cmd, point, pipeline);
}

inline void bind_descriptor_sets(VkCommandBuffer cmd, VkPipelineBindPoint point,
                                 VkPipelineLayout layout, u32 first_set, u32 set_count,
                                 const VkDescriptorSet* sets, u32 dynamic_offset_count,
                                 const u32* dynamic_offsets) {
    command_counts().descriptor_set_binds += set_count;
    vkCmdBindDescriptorSets(cmd, point, layout, first_set, set_count, sets, dynamic_offset_count,
                            dynamic_offsets);
}

inline void push_constants(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages,
                           u32 offset, u32 size, const void* values) {
    ++command_counts().push_constants;
    vkCmdPushConstants(cmd, layout, stages, offset, size, values);
}

inline void update_descriptor_sets(VkDevice device, u32 write_count,
                                   const VkWriteDescriptorSet* writes, u32 copy_count,
                                   const VkCopyDescriptorSet* copies) {
    command_counts().descriptor_writes += write_count;
    vkUpdateDescriptorSets(device, write_count, writes, copy_count, copies);
}

} // namespace vkc
} // namespace osc::renderer
