#include "renderer/boundary_renderer.hpp"

#include "renderer/shader_utils.hpp"
#include "renderer/stencil_volume.hpp"
#include "renderer/vk_cmd.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cstring>
#include <utility>

namespace osc::renderer {

namespace {

/// A box pass's constants: the view-projection, the box's centre (w: the
/// pass's depth bias) and half extent.
struct BoxPush {
    f32 view_proj[16];
    f32 center[4];
    f32 extent[4];
};

// boundaryVertexShader: the unit box's corner scaled by the half extent
// about the centre. The box is made here: its 12 triangles, each wound
// counter-clockwise seen from outside (front faces face out). The pass's
// DepthBias (1e-5, of the 0-to-1 depth) is added after the projection.
const char* const kBoxVert = R"glsl(
#version 450

layout(push_constant) uniform Push {
    mat4 view_proj;
    vec4 center;
    vec4 extent;
} pc;

const int kCorners[36] = int[36](0, 4, 6, 0, 6, 2,  1, 3, 7, 1, 7, 5,
                                 0, 1, 5, 0, 5, 4,  2, 6, 7, 2, 7, 3,
                                 0, 2, 3, 0, 3, 1,  4, 5, 7, 4, 7, 6);

void main() {
    int c = kCorners[gl_VertexIndex];
    vec3 corner = vec3((c & 1) != 0 ? 1.0 : -1.0, (c & 2) != 0 ? 1.0 : -1.0,
                       (c & 4) != 0 ? 1.0 : -1.0);
    vec4 p = pc.view_proj * vec4(pc.extent.xyz * corner + pc.center.xyz, 1.0);
    p.z += pc.center.w * p.w;
    gl_Position = p;
}
)glsl";

/// BoundaryEdgesDiffer: an edge off by a squared 0.001 or more
bool differs(f32 map_edge, f32 playable_edge) {
    const f32 d = map_edge - playable_edge;
    return d * d >= 0.001f;
}

/// CW_BOUNDARY_BIAS and CCW_BOUNDARY_BIAS: back faces nearer, front farther
constexpr f32 kFrontBias = 0.00001f;
constexpr f32 kBackBias = -0.00001f;

} // namespace

void BoundaryRenderer::init(VkDevice device, VkRenderPass render_pass) {
    using namespace stencil_volume;
    cast_layout_ = make_layout(device, VK_SHADER_STAGE_VERTEX_BIT, sizeof(BoxPush));
    screen_layout_ = make_layout(device, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(f32) * 4);
    VkShaderModule box_vert = compile_glsl(device, kBoxVert, "boundary_box.vert", true);
    VkShaderModule cast_frag = compile_glsl(device, kCastFrag, "boundary_cast.frag", false);
    VkShaderModule screen_vert = compile_glsl(device, kScreenVert, "boundary_screen.vert", true);
    VkShaderModule screen_frag = compile_glsl(device, kScreenFrag, "boundary_screen.frag", false);
    if (box_vert && cast_frag && screen_vert && screen_frag) {
        constexpr auto keep = VK_STENCIL_OP_KEEP;
        // A cast pass: one facing, ZFunc less, nothing written but the
        // stencil, which counts up or down where the face fails the depth
        const auto pass = [&](VkCullModeFlags cull, VkStencilOp depth_fail) {
            PassState s{true};
            s.ring_input = false;
            s.cull = cull;
            s.front = s.back =
                stencil(VK_COMPARE_OP_ALWAYS, 0xFF, 0x00, 0xFF, keep, keep, depth_fail);
            return make_pipeline(device, render_pass, cast_layout_, box_vert, cast_frag, s);
        };
        front_up_ = pass(VK_CULL_MODE_BACK_BIT, VK_STENCIL_OP_INCREMENT_AND_CLAMP);
        front_down_ = pass(VK_CULL_MODE_BACK_BIT, VK_STENCIL_OP_DECREMENT_AND_CLAMP);
        back_up_ = pass(VK_CULL_MODE_FRONT_BIT, VK_STENCIL_OP_INCREMENT_AND_CLAMP);
        back_down_ = pass(VK_CULL_MODE_FRONT_BIT, VK_STENCIL_OP_DECREMENT_AND_CLAMP);
        // Boundary: black at alpha 1 over the colour where the count isn't 0
        PassState black;
        black.front = black.back =
            stencil(VK_COMPARE_OP_NOT_EQUAL, 0xFF, 0x00, 0x00, keep, keep, keep);
        black.write =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        black.blend = true;
        black_ =
            make_pipeline(device, render_pass, screen_layout_, screen_vert, screen_frag, black);
    } else {
        spdlog::error("BoundaryRenderer: shader compilation failed");
    }
    for (VkShaderModule m : {box_vert, cast_frag, screen_vert, screen_frag})
        if (m) vkDestroyShaderModule(device, m, nullptr);
}

void BoundaryRenderer::update(f32 map_width, f32 map_height,
                              const std::optional<PlayableRect>& playable) {
    map_ = {0.0f, 0.0f, map_width, map_height};
    if (!playable) {
        drawn_ = false;
        return;
    }
    playable_ = {static_cast<f32>(playable->x0), static_cast<f32>(playable->z0),
                 static_cast<f32>(playable->x1), static_cast<f32>(playable->z1)};
    drawn_ = differs(map_[0], playable_[0]) || differs(map_[2], playable_[2]) ||
             differs(map_[1], playable_[1]) || differs(map_[3], playable_[3]);
}

void BoundaryRenderer::cast(VkCommandBuffer cmd, const f32* view_proj, const Rect& box,
                            VkPipeline first, f32 first_bias, VkPipeline second,
                            f32 second_bias) const {
    BoxPush push{};
    std::memcpy(push.view_proj, view_proj, sizeof(push.view_proj));
    push.center[0] = (box[0] + box[2]) * 0.5f;
    push.center[1] = (kFloor + kCeiling) * 0.5f;
    push.center[2] = (box[1] + box[3]) * 0.5f;
    push.extent[0] = (box[2] - box[0]) * 0.5f;
    push.extent[1] = (kCeiling - kFloor) * 0.5f;
    push.extent[2] = (box[3] - box[1]) * 0.5f;
    const std::array<std::pair<VkPipeline, f32>, 2> passes = {
        {{first, first_bias}, {second, second_bias}}};
    for (const auto& [pipeline, bias] : passes) {
        push.center[3] = bias;
        vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkc::push_constants(cmd, cast_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        vkc::draw(cmd, 36, 1, 0, 0);
    }
}

void BoundaryRenderer::render(VkCommandBuffer cmd, const VkRect2D& area,
                              const f32* view_proj) const {
    if (!drawn_ || !front_up_ || !front_down_ || !back_up_ || !back_down_ || !black_) {
        return;
    }
    vkc::set_area(cmd, area);

    // The stencil cleared first (RenderPlayableBoundary's Clear)
    VkClearAttachment clear{};
    clear.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    clear.clearValue.depthStencil = {1.0f, 0};
    VkClearRect rect{};
    rect.rect = area;
    rect.layerCount = 1;
    vkCmdClearAttachments(cmd, 1, &clear, 1, &rect);

    // CastBoundaryCW over the map: its back faces up, then its front faces
    // down; CastBoundaryCCW over the playable rect: front up, then back down
    cast(cmd, view_proj, map_, back_up_, kBackBias, front_down_, kFrontBias);
    cast(cmd, view_proj, playable_, front_up_, kFrontBias, back_down_, kBackBias);

    constexpr std::array<f32, 4> kBlack = {0.0f, 0.0f, 0.0f, 1.0f};
    vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, black_);
    vkc::push_constants(cmd, screen_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(kBlack),
                        kBlack.data());
    vkc::draw(cmd, 3, 1, 0, 0);
}

void BoundaryRenderer::destroy(VkDevice device) {
    for (VkPipeline* p : {&front_up_, &front_down_, &back_up_, &back_down_, &black_}) {
        if (*p) vkDestroyPipeline(device, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    for (VkPipelineLayout* l : {&cast_layout_, &screen_layout_}) {
        if (*l) vkDestroyPipelineLayout(device, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
    drawn_ = false;
}

} // namespace osc::renderer
