#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace osc::renderer {

/// Fluent builder for VkPipeline — reduces ~100 lines of Vulkan boilerplate to ~10.
class PipelineBuilder {
public:
    PipelineBuilder& set_shaders(VkShaderModule vert, VkShaderModule frag);
    PipelineBuilder& set_vertex_input(
        const VkVertexInputBindingDescription* bindings, uint32_t binding_count,
        const VkVertexInputAttributeDescription* attrs, uint32_t attr_count);
    PipelineBuilder& set_topology(VkPrimitiveTopology topo);
    PipelineBuilder& set_cull_mode(VkCullModeFlags cull, VkFrontFace front);
    PipelineBuilder& set_depth_test(bool test, bool write);
    PipelineBuilder& set_blend(bool enable);
    PipelineBuilder& set_push_constant(uint32_t size,
                                       VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT);
    PipelineBuilder& set_descriptor_set_layout(VkDescriptorSetLayout layout);
    PipelineBuilder& add_descriptor_set_layout(VkDescriptorSetLayout layout);

    /// Build the pipeline. Returns VK_NULL_HANDLE on failure.
    VkPipeline build(VkDevice device, VkRenderPass render_pass,
                     VkPipelineLayout* out_layout);

private:
    VkShaderModule vert_ = VK_NULL_HANDLE;
    VkShaderModule frag_ = VK_NULL_HANDLE;

    const VkVertexInputBindingDescription* bindings_ = nullptr;
    uint32_t binding_count_ = 0;
    const VkVertexInputAttributeDescription* attrs_ = nullptr;
    uint32_t attr_count_ = 0;

    VkPrimitiveTopology topology_ = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkCullModeFlags cull_mode_ = VK_CULL_MODE_BACK_BIT;
    VkFrontFace front_face_ = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool depth_test_ = true;
    bool depth_write_ = true;
    bool blend_ = false;
    uint32_t push_constant_size_ = 0;
    VkShaderStageFlags push_constant_stages_ = VK_SHADER_STAGE_VERTEX_BIT;
    std::vector<VkDescriptorSetLayout> ds_layouts_;
};

} // namespace osc::renderer
