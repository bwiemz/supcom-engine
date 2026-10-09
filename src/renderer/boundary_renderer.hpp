#pragma once

// The playable area's boundary (Moho's RenderPlayableBoundary, faf-re
// render/BoundaryRenderer.cpp; retail's vision.fx CastBoundaryCW and
// CastBoundaryCCW, frame.fx's Boundary): where the playable rect is smaller
// than the map, what lies outside it is blacked out.
// - The stencil cleared, the map's box (its rect from 256 below to 250
//   above) is cast z-fail with its back faces counting up, then its front
//   faces down; then the playable rect's box the other way round, front
//   faces up then back down (each pass its own, as the counts saturate).
//   What the depth so far holds inside the map's box and outside the
//   playable one is left counted.
// - Boundary: black at alpha 1 over the colour where the count isn't 0.
// Nothing is drawn when the two rects match. A depth-and-stencil pass's;
// the world's after the water, and a cartographic frame's after its fog.

#include "core/types.hpp"
#include "renderer/playable_rect.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <optional>

namespace osc::renderer {

class BoundaryRenderer {
public:
    void init(VkDevice device, VkRenderPass render_pass);

    /// The map's size and the playable rect (none: the whole map): drawn
    /// only where an edge of the one is off the other's (by a hundredth of a
    /// unit or more, BoundaryEdgesDiffer's squared 0.001).
    void update(f32 map_width, f32 map_height, const std::optional<PlayableRect>& playable);
    /// Nothing drawn (an editor's or a test's).
    void set_off() { drawn_ = false; }

    /// Into the open pass, which must have a stencil.
    void render(VkCommandBuffer cmd, const VkRect2D& area, const f32* view_proj) const;

    /// Whether the last update() found a boundary to draw.
    bool drawn() const { return drawn_; }

    void destroy(VkDevice device);

    /// The boxes' heights (kBoundaryVolumeFloor, kBoundaryVolumeCeiling).
    static constexpr f32 kFloor = -256.0f;
    static constexpr f32 kCeiling = 250.0f;

private:
    /// A box's corners: x0, z0, x1, z1.
    using Rect = std::array<f32, 4>;
    void cast(VkCommandBuffer cmd, const f32* view_proj, const Rect& box, VkPipeline first,
              f32 first_bias, VkPipeline second, f32 second_bias) const;

    VkPipelineLayout cast_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout screen_layout_ = VK_NULL_HANDLE;
    /// The casts' passes: front or back faces, counting up or down on a
    /// depth fail (saturating, as D3D's INCR and DECR)
    VkPipeline front_up_ = VK_NULL_HANDLE;
    VkPipeline front_down_ = VK_NULL_HANDLE;
    VkPipeline back_up_ = VK_NULL_HANDLE;
    VkPipeline back_down_ = VK_NULL_HANDLE;
    VkPipeline black_ = VK_NULL_HANDLE;
    Rect map_{};
    Rect playable_{};
    bool drawn_ = false;
};

} // namespace osc::renderer
