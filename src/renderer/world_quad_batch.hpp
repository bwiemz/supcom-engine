#pragma once

#include "core/types.hpp"
#include "renderer/vk_types.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <vector>

namespace osc::renderer {

/// Textured quads in the world, drawn over it as primbatcher.fx's TCommand:
/// texture × colour, alpha blended, no depth test, no alpha written.
class WorldQuadBatch {
public:
    struct Vertex {
        f32 pos[3];
        f32 uv[2];
        f32 color[4];
    };
    struct Quad {
        VkDescriptorSet ds = VK_NULL_HANDLE;
        std::array<Vertex, 6> v;
    };

    void init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
              VkDescriptorSetLayout texture_ds_layout, u32 max_quads, const char* name);

    /// This frame's quads, drawn in order (at most the batch's max)
    void upload(const std::vector<Quad>& quads, u32 fi);

    void render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h, const f32* view_proj,
                u32 fi) const;

    void destroy(VkDevice device, VmaAllocator allocator);

    bool ready(u32 fi) const { return vertex_mapped_[fi] != nullptr; }

    static constexpr u32 FRAMES_IN_FLIGHT = 2;

private:
    struct Group {
        VkDescriptorSet ds = VK_NULL_HANDLE;
        u32 first_vertex = 0, vertex_count = 0;
    };

    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    AllocatedBuffer vertex_buf_[FRAMES_IN_FLIGHT] = {};
    void* vertex_mapped_[FRAMES_IN_FLIGHT] = {};
    u32 max_quads_ = 0;
    std::vector<Group> groups_;
};

} // namespace osc::renderer
