#include "renderer/projectile_icon_renderer.hpp"

#include "renderer/pipeline_builder.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/vk_cmd.hpp"

#include <spdlog/spdlog.h>

#include <cstddef>

namespace osc::renderer {

namespace {

struct PushConstants {
    f32 viewport[2];
    f32 glow;
};

} // namespace

void ProjectileIconRenderer::init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
                                  VkDescriptorSetLayout texture_ds_layout) {
    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = MAX_ICONS * sizeof(Instance);
    buf_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    alloc_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        VmaAllocationInfo info{};
        vmaCreateBuffer(allocator, &buf_info, &alloc_info, &instance_buf_[i].buffer,
                        &instance_buf_[i].allocation, &info);
        instance_mapped_[i] = info.pMappedData;
    }

    VkShaderModule vert =
        compile_glsl(device, shaders::projectile_icon_vert, "projectile_icon.vert", true);
    VkShaderModule frag =
        compile_glsl(device, shaders::projectile_icon_frag, "projectile_icon.frag", false);
    if (vert && frag) {
        VkVertexInputBindingDescription binding{};
        binding.stride = sizeof(Instance);
        binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        const VkVertexInputAttributeDescription attrs[2] = {
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, rect)},
            {1, 0, VK_FORMAT_R32_SFLOAT, offsetof(Instance, glow)}};
        const auto pipeline = [&](bool blend, VkColorComponentFlags mask,
                                  VkPipelineLayout* layout) {
            PipelineBuilder builder;
            builder.set_shaders(vert, frag)
                .set_vertex_input(&binding, 1, attrs, 2)
                .set_depth_test(false, false)
                .set_blend(blend)
                .set_color_write_mask(mask)
                .set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .set_push_constant(sizeof(PushConstants),
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                .set_descriptor_set_layout(texture_ds_layout);
            if (blend) {
                builder.set_alpha_blend(VK_BLEND_FACTOR_SRC_ALPHA,
                                        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
            }
            return builder.build(device, render_pass, layout);
        };
        colour_pipeline_ = pipeline(
            true, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT,
            &colour_layout_);
        glow_pipeline_ = pipeline(false, VK_COLOR_COMPONENT_A_BIT, &glow_layout_);
    } else {
        spdlog::error("ProjectileIconRenderer: shader compilation failed");
    }
    if (vert) {
        vkDestroyShaderModule(device, vert, nullptr);
    }
    if (frag) {
        vkDestroyShaderModule(device, frag, nullptr);
    }
}

void ProjectileIconRenderer::update(std::span<const ProjectileIcon> icons, u32 fi) {
    groups_.clear();
    auto* out = static_cast<Instance*>(instance_mapped_[fi]);
    if (!out) {
        return;
    }
    u32 count = 0;
    for (const ProjectileIcon& icon : icons) {
        if (count >= MAX_ICONS) {
            break;
        }
        out[count] = {{icon.x, icon.y, icon.w, icon.h}, icon.glow};
        if (groups_.empty() || groups_.back().ds != icon.ds) {
            groups_.push_back({icon.ds, count, 0});
        }
        ++groups_.back().count;
        ++count;
    }
}

void ProjectileIconRenderer::render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h,
                                    u32 fi) const {
    if (groups_.empty() || !colour_pipeline_ || !glow_pipeline_ || !instance_buf_[fi].buffer) {
        return;
    }
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &instance_buf_[fi].buffer, &offset);
    for (const bool glow : {false, true}) {
        VkPipelineLayout layout = glow ? glow_layout_ : colour_layout_;
        vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                           glow ? glow_pipeline_ : colour_pipeline_);
        const PushConstants pc{{static_cast<f32>(viewport_w), static_cast<f32>(viewport_h)},
                               glow ? 1.0f : 0.0f};
        vkc::push_constants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(pc), &pc);
        for (const Group& g : groups_) {
            vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &g.ds, 0,
                                      nullptr);
            vkc::draw(cmd, 6, g.count, 0, g.first);
        }
    }
}

void ProjectileIconRenderer::destroy(VkDevice device, VmaAllocator allocator) {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        if (instance_buf_[i].buffer) {
            vmaDestroyBuffer(allocator, instance_buf_[i].buffer, instance_buf_[i].allocation);
        }
        instance_buf_[i] = {};
        instance_mapped_[i] = nullptr;
    }
    for (VkPipeline* p : {&colour_pipeline_, &glow_pipeline_}) {
        if (*p) {
            vkDestroyPipeline(device, *p, nullptr);
        }
        *p = VK_NULL_HANDLE;
    }
    for (VkPipelineLayout* l : {&colour_layout_, &glow_layout_}) {
        if (*l) {
            vkDestroyPipelineLayout(device, *l, nullptr);
        }
        *l = VK_NULL_HANDLE;
    }
}

} // namespace osc::renderer
