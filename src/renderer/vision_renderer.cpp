#include "renderer/vision_renderer.hpp"

#include "renderer/shader_utils.hpp"
#include "renderer/stencil_volume.hpp"
#include "renderer/vk_cmd.hpp"
#include "sim/world_snapshot.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace osc::renderer {

using stencil_volume::CastPush;

std::vector<RangeRing> vision_circles(const sim::FrameView& view, i32 focus_army) {
    std::vector<RangeRing> circles;
    const sim::WorldSnapshot* snap = view.cur();
    if (!snap || focus_army < 0) return circles;
    const sim::ArmyRecord* focus = snap->army(focus_army);
    const u32 allies = focus ? focus->allies : 0u;
    // UserArmy::IsAlly: its own army, and those it calls allies
    const auto allied = [&](i32 army) {
        return army == focus_army || (army >= 0 && army < 32 && (allies >> army & 1u) != 0);
    };
    for (const sim::EntityRecord& e : view.entities()) {
        if (!e.is_unit || e.is_being_built || e.is_dying || !allied(e.army)) continue;
        for (const sim::IntelRecord& intel : snap->intel_of(e)) {
            if (intel.type != "Vision") continue;
            // The intel's range is kept in whole units
            const f32 radius = std::floor(intel.radius);
            if (radius <= 0.0f) break;
            const sim::Vector3 at = view.position(e);
            circles.push_back({at.x, at.z, 0.0f, radius});
            break;
        }
    }
    return circles;
}

void VisionRenderer::init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass) {
    using namespace stencil_volume;
    // The cylinder: the ring volume's template, its height 0 at the bottom,
    // 1 at the top (a circle is a ring with no hole)
    const std::vector<RingVertex> volume = ring_volume_vertices(0.0f, 1.0f);
    const std::vector<u16> indices = ring_volume_indices();
    index_count_ = static_cast<u32>(indices.size());
    make_buffer(allocator, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, sizeof(RingVertex) * volume.size(),
                volume_, volume.data());
    make_buffer(allocator, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, sizeof(u16) * indices.size(), indices_,
                indices.data());
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i)
        circle_mapped_[i] = make_buffer(allocator, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                        sizeof(RangeRing) * MAX_CIRCLES, circle_buf_[i]);

    cast_layout_ = make_layout(device, VK_SHADER_STAGE_VERTEX_BIT, sizeof(CastPush));
    screen_layout_ = make_layout(device, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(f32) * 4);

    VkShaderModule cast_vert = compile_glsl(device, kCastVert, "vision_cast.vert", true);
    VkShaderModule cast_frag = compile_glsl(device, kCastFrag, "vision_cast.frag", false);
    VkShaderModule screen_vert = compile_glsl(device, kScreenVert, "vision_screen.vert", true);
    VkShaderModule screen_frag = compile_glsl(device, kScreenFrag, "vision_screen.frag", false);
    if (cast_vert && cast_frag && screen_vert && screen_frag) {
        constexpr auto keep = VK_STENCIL_OP_KEEP;
        // CastVision: where the depth so far lies behind a face, its front
        // faces count up and its back faces down (incr/decr wrap), nothing
        // written but the stencil
        PassState cast{true};
        cast.front = stencil(VK_COMPARE_OP_ALWAYS, 0xFF, 0x00, 0xFF, keep, keep,
                             VK_STENCIL_OP_INCREMENT_AND_WRAP);
        cast.back = cast.front;
        cast.back.depthFailOp = VK_STENCIL_OP_DECREMENT_AND_WRAP;
        cast_ = make_pipeline(device, render_pass, cast_layout_, cast_vert, cast_frag, cast);
        // Vision: black at alpha 0.33 over the colour where no circle counted
        PassState fog;
        fog.front = fog.back = stencil(VK_COMPARE_OP_EQUAL, 0xFF, 0x00, 0x00, keep, keep, keep);
        fog.write = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        fog.blend = true;
        fog_ = make_pipeline(device, render_pass, screen_layout_, screen_vert, screen_frag, fog);
    } else {
        spdlog::error("VisionRenderer: shader compilation failed");
    }
    for (VkShaderModule m : {cast_vert, cast_frag, screen_vert, screen_frag})
        if (m) vkDestroyShaderModule(device, m, nullptr);
}

void VisionRenderer::update(const std::vector<RangeRing>& circles, u32 fi) {
    circles_ = circles;
    drawn_ = true;
    if (circles_.size() > MAX_CIRCLES) {
        if (!warned_full_) spdlog::warn("VisionRenderer: over {} vision circles", MAX_CIRCLES);
        warned_full_ = true;
        circles_.resize(MAX_CIRCLES);
    }
    if (circle_mapped_[fi] && !circles_.empty())
        std::memcpy(circle_mapped_[fi], circles_.data(), sizeof(RangeRing) * circles_.size());
}

void VisionRenderer::render(VkCommandBuffer cmd, const VkRect2D& area, const f32* view_proj,
                            u32 fi) const {
    if (!drawn_ || !cast_ || !fog_ || !circle_buf_[fi].buffer) return;
    vkc::set_area(cmd, area);

    // The stencil cleared first (RenderFogOfWar's Clear)
    VkClearAttachment clear{};
    clear.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    clear.clearValue.depthStencil = {1.0f, 0};
    VkClearRect rect{};
    rect.rect = area;
    rect.layerCount = 1;
    vkCmdClearAttachments(cmd, 1, &clear, 1, &rect);

    if (!circles_.empty()) {
        CastPush push{};
        std::memcpy(push.view_proj, view_proj, sizeof(push.view_proj));
        push.heights[0] = bottom_;
        push.heights[1] = top_;
        vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cast_);
        vkc::push_constants(cmd, cast_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        const std::array<VkBuffer, 2> buffers = {volume_.buffer, circle_buf_[fi].buffer};
        const std::array<VkDeviceSize, 2> offsets = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers.data(), offsets.data());
        vkCmdBindIndexBuffer(cmd, indices_.buffer, 0, VK_INDEX_TYPE_UINT16);
        vkc::draw_indexed(cmd, index_count_, static_cast<u32>(circles_.size()), 0, 0, 0);
    }
    constexpr std::array<f32, 4> kFog = {0.0f, 0.0f, 0.0f, kFogAlpha};
    vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fog_);
    vkc::push_constants(cmd, screen_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(kFog),
                        kFog.data());
    vkc::draw(cmd, 3, 1, 0, 0);
}

void VisionRenderer::destroy(VkDevice device, VmaAllocator allocator) {
    for (VkPipeline* p : {&cast_, &fog_}) {
        if (*p) vkDestroyPipeline(device, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    for (VkPipelineLayout* l : {&cast_layout_, &screen_layout_}) {
        if (*l) vkDestroyPipelineLayout(device, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
    const auto free = [allocator](AllocatedBuffer& b) {
        if (b.buffer) vmaDestroyBuffer(allocator, b.buffer, b.allocation);
        b = {};
    };
    free(volume_);
    free(indices_);
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        free(circle_buf_[i]);
        circle_mapped_[i] = nullptr;
    }
    circles_.clear();
    drawn_ = false;
}

} // namespace osc::renderer
