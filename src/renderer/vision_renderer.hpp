#pragma once

// FA's drawn fog of war (Moho's RenderFogOfWar, faf-re
// render/VisionRenderer.cpp; retail's vision.fx CastVision and frame.fx
// Vision): each vision circle of the user's VisionDB is cast into the
// stencil as a cylinder against the depth so far (z-fail, wrapping counts),
// then what no circle holds is darkened to 67% (black at alpha 0.33, colour
// only). Two states, no explored ground. Moho skips the pass for an
// observer and with ren_FogOfWar off.

#include "core/types.hpp"
#include "renderer/range_overlays.hpp"
#include "renderer/vk_types.hpp"

#include <vulkan/vulkan.h>

#include <vector>

namespace osc::sim {
class FrameView;
}

namespace osc::renderer {

/// The focus army's vision circles this frame, as Moho's VisionDB gives them
/// to RenderFogOfWar (UserUnit::Tick's handle): every unit with a vision
/// radius whose army is the focus army or one it calls an ally, at its
/// position interpolated between the ticks, its radius the whole units of
/// its vision. Not one being built, dying or gone; the vision's toggle
/// doesn't matter. Each a ring with no hole (`inner` 0), for the cylinder.
/// None for an observer (`focus_army` -1).
std::vector<RangeRing> vision_circles(const sim::FrameView& view, i32 focus_army);

class VisionRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass);

    /// The map's height range: the cylinders reach from 5 below its lowest
    /// point to its highest, as the range volumes do.
    void set_heights(f32 lowest, f32 highest) {
        bottom_ = lowest - 5.0f;
        top_ = highest;
    }

    /// This frame's circles (none: all of it fogged).
    void update(const std::vector<RangeRing>& circles, u32 fi);
    /// No fog this frame (an observer's, or with the fog off).
    void set_off() {
        circles_.clear();
        drawn_ = false;
    }

    /// The fog, in a pass with a depth buffer that has a stencil; the
    /// stencil is cleared first and left as the circles counted it.
    void render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h, const f32* view_proj,
                u32 fi) const;

    /// What the last update() drew (tests read it)
    const std::vector<RangeRing>& circles() const { return circles_; }
    bool drawn() const { return drawn_; }

    void destroy(VkDevice device, VmaAllocator allocator);

    static constexpr u32 FRAMES_IN_FLIGHT = 2;
    /// Circles a frame (Moho's batches have no limit; this is more than a
    /// full game's units)
    static constexpr u32 MAX_CIRCLES = 16384;
    /// frame.fx's Vision: VisionPS(0.33)
    static constexpr f32 kFogAlpha = 0.33f;

private:
    VkPipeline cast_ = VK_NULL_HANDLE;
    VkPipeline fog_ = VK_NULL_HANDLE;
    VkPipelineLayout cast_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout screen_layout_ = VK_NULL_HANDLE;
    AllocatedBuffer volume_ = {};
    AllocatedBuffer indices_ = {};
    u32 index_count_ = 0;
    AllocatedBuffer circle_buf_[FRAMES_IN_FLIGHT] = {};
    void* circle_mapped_[FRAMES_IN_FLIGHT] = {};
    f32 bottom_ = -5.0f, top_ = 256.0f;
    std::vector<RangeRing> circles_;
    bool drawn_ = false;
    bool warned_full_ = false;
};

} // namespace osc::renderer
