#include "renderer/resource_icon_renderer.hpp"

#include "renderer/camera.hpp"
#include "renderer/decal_math.hpp"
#include "renderer/pipeline_builder.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/texture_cache.hpp"
#include "renderer/vk_cmd.hpp"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstring>

namespace osc::renderer {

namespace {

struct PushConstants {
    f32 viewport[2];
    f32 time;
    f32 glow;
};

constexpr f32 kScreenMargin = 32.0f;

std::array<i32, 2> footprint_origin(const sim::ResourceDeposit& d) {
    const auto size = static_cast<i32>(d.size);
    return {static_cast<i32>(d.x - static_cast<f32>(size) * 0.5f),
            static_cast<i32>(d.z - static_cast<f32>(size) * 0.5f)};
}

} // namespace

std::array<f32, 2> deposit_centre(const sim::ResourceDeposit& deposit) {
    const auto size = static_cast<f32>(static_cast<i32>(deposit.size));
    const auto [x0, z0] = footprint_origin(deposit);
    return {size * 0.5f + static_cast<f32>(x0), size * 0.5f + static_cast<f32>(z0)};
}

bool deposit_in_rect(const sim::ResourceDeposit& deposit, const PlayableRect& rect) {
    const auto size = static_cast<i32>(deposit.size);
    const auto [x0, z0] = footprint_origin(deposit);
    return x0 >= rect.x0 && x0 + size <= rect.x1 && z0 >= rect.z0 && z0 + size <= rect.z1;
}

std::vector<ResourceIcon> resource_icons(std::span<const sim::ResourceDeposit> deposits,
                                         const Camera& camera, f32 width, f32 height,
                                         const PlayableRect& playable,
                                         const std::function<f32(f32, f32)>& ground) {
    std::vector<ResourceIcon> icons;
    if (width <= 0.0f || height <= 0.0f) {
        return icons;
    }
    const f32 aspect = width / height;
    const std::array<f32, 16> view = camera.view();
    const std::array<f32, 16> vp = camera.view_proj(aspect);
    const f32 half_width = camera.tan_half_fov_y(aspect) * aspect;
    std::array<f32, 3> eye{};
    camera.eye_position(eye[0], eye[1], eye[2]);
    for (const sim::ResourceDeposit& d : deposits) {
        if (!deposit_in_rect(d, playable)) {
            continue;
        }
        const auto [x, z] = deposit_centre(d);
        const f32 y = ground(x, z);
        if (decal_lod_metric(view, eye, half_width, x, y, z) <= kResourceLodCutoff) {
            continue;
        }
        const f32 cx = vp[0] * x + vp[4] * y + vp[8] * z + vp[12];
        const f32 cy = vp[1] * x + vp[5] * y + vp[9] * z + vp[13];
        const f32 cw = vp[3] * x + vp[7] * y + vp[11] * z + vp[15];
        if (cw <= 0.001f) {
            continue;
        }
        const f32 sx = (cx / cw + 1.0f) * 0.5f * width;
        const f32 sy = (cy / cw + 1.0f) * 0.5f * height;
        if (sx < -kScreenMargin || sx > width + kScreenMargin || sy < -kScreenMargin ||
            sy > height + kScreenMargin) {
            continue;
        }
        icons.push_back({std::floor(sx), std::floor(sy), d.type});
    }
    return icons;
}

f32 resource_icon_glow(f32 alpha, f32 time, f32 x, f32 y) {
    const f32 radius = 0.00277f * std::sqrt(x * x + y * y);
    const f32 sine = std::sin(time - 10.0f * radius);
    return alpha * 0.6f * sine * sine;
}

void ResourceIconRenderer::init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
                                VkDescriptorSetLayout texture_ds_layout) {
    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = MAX_ICONS * sizeof(std::array<f32, 4>);
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
        compile_glsl(device, shaders::resource_icon_vert, "resource_icon.vert", true);
    VkShaderModule frag =
        compile_glsl(device, shaders::resource_icon_frag, "resource_icon.frag", false);
    if (vert && frag) {
        VkVertexInputBindingDescription binding{};
        binding.stride = sizeof(std::array<f32, 4>);
        binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        const VkVertexInputAttributeDescription attr{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
        const auto pipeline = [&](VkColorComponentFlags mask, VkPipelineLayout* layout) {
            return PipelineBuilder()
                .set_shaders(vert, frag)
                .set_vertex_input(&binding, 1, &attr, 1)
                .set_depth_test(false, false)
                .set_blend(true)
                .set_alpha_blend(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA)
                .set_color_write_mask(mask)
                .set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .set_push_constant(sizeof(PushConstants),
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                .set_descriptor_set_layout(texture_ds_layout)
                .build(device, render_pass, layout);
        };
        colour_pipeline_ =
            pipeline(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT,
                     &colour_layout_);
        glow_pipeline_ = pipeline(VK_COLOR_COMPONENT_A_BIT, &glow_layout_);
    } else {
        spdlog::error("ResourceIconRenderer: shader compilation failed");
    }
    if (vert) {
        vkDestroyShaderModule(device, vert, nullptr);
    }
    if (frag) {
        vkDestroyShaderModule(device, frag, nullptr);
    }
}

void ResourceIconRenderer::update(std::span<const ResourceIcon> icons, TextureCache& tex_cache,
                                  u32 fi) {
    quads_.clear();
    groups_.clear();
    for (const auto type : {sim::ResourceDeposit::Mass, sim::ResourceDeposit::Hydrocarbon}) {
        const GPUTexture* tex = tex_cache.get(
            type == sim::ResourceDeposit::Mass ? kMassIconTexture : kHydrocarbonIconTexture);
        if (!tex || tex->width == 0) {
            continue;
        }
        const auto half_w = static_cast<f32>(tex->width >> 2);
        const auto half_h = static_cast<f32>(tex->height >> 2);
        Group group{tex->descriptor_set, static_cast<u32>(quads_.size()), 0};
        for (const ResourceIcon& icon : icons) {
            if (icon.type != type || quads_.size() >= MAX_ICONS) {
                continue;
            }
            quads_.push_back({icon.x - half_w, icon.y - half_h, 2.0f * half_w, 2.0f * half_h});
            ++group.count;
        }
        if (group.count > 0) {
            groups_.push_back(group);
        }
    }
    if (!quads_.empty() && instance_mapped_[fi]) {
        std::memcpy(instance_mapped_[fi], quads_.data(), quads_.size() * sizeof(quads_[0]));
    }
}

void ResourceIconRenderer::render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h, f32 time,
                                  u32 fi) const {
    if (groups_.empty() || !colour_pipeline_ || !glow_pipeline_ || !instance_buf_[fi].buffer) {
        return;
    }
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &instance_buf_[fi].buffer, &offset);
    for (const Group& g : groups_) {
        for (const bool glow : {false, true}) {
            VkPipelineLayout layout = glow ? glow_layout_ : colour_layout_;
            vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                               glow ? glow_pipeline_ : colour_pipeline_);
            const PushConstants pc{{static_cast<f32>(viewport_w), static_cast<f32>(viewport_h)},
                                   time,
                                   glow ? 1.0f : 0.0f};
            vkc::push_constants(cmd, layout,
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                sizeof(pc), &pc);
            vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &g.ds, 0,
                                      nullptr);
            vkc::draw(cmd, 6, g.count, 0, g.first);
        }
    }
}

void ResourceIconRenderer::destroy(VkDevice device, VmaAllocator allocator) {
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
