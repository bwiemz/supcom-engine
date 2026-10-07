#pragma once

#include "core/types.hpp"
#include "renderer/range_overlays.hpp"
#include "renderer/vk_types.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <string>
#include <unordered_set>
#include <vector>

struct lua_State;

namespace osc::renderer {

class Frustum;

/// What the focus army's range overlays are drawn for this frame: its units
/// in view (alive and built), its selected units, the unit under the cursor
/// if its, a structure being placed (`placing`, at the cursor), and its
/// no-rush zone; each only when a pass of `overlays` reads it. Blueprints
/// from the UI state `L`.
RangeScene collect_range_scene(const RangeOverlays& overlays, const sim::FrameView& view,
                               const Frustum& frustum, i32 focus_army,
                               const std::unordered_set<u32>* selected, u32 hovered,
                               const std::string* placing, f32 cursor_x, f32 cursor_z,
                               RangeBlueprints& blueprints, lua_State* L);

/// Draws range batches as Moho's RenderRingBatch does: each ring a stencil
/// volume on the depth so far (range.fx's Cast), its fills counted then
/// marked (frame.fx's RangeMask), its lines counted outside every fill, the
/// fills tinted (RangeFill, with range_Fill) and the lines burned in the
/// batch's colour (RangeBurn), the stencil cleared. Needs a depth buffer
/// with a stencil.
class RangeRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass);

    /// The map's height range: the volumes reach from 5 below its lowest
    /// point to its highest (FAF's Sim::Create_exxt).
    void set_heights(f32 lowest, f32 highest) {
        bottom_ = lowest - 5.0f;
        top_ = highest;
    }

    /// This frame's batches, their lines `ring_thickness` thick at
    /// `zoom_ratio` on a map whose playable rect's larger side is `span`.
    void update(const std::vector<RangeBatch>& batches, const RangeOverlays::Settings& settings,
                f32 span, f32 zoom_ratio, u32 fi);

    void render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h, const f32* view_proj,
                u32 fi) const;

    void destroy(VkDevice device, VmaAllocator allocator);

    /// A batch as drawn: its bands' places among the frame's rings
    struct Drawn {
        RangeColor color{};
        f32 inner_thickness = 0, outer_thickness = 0;
        u32 fill_first = 0, fill_count = 0;
        u32 edge_first = 0, edge_count = 0;
    };
    /// What the last update() laid out (tests read it)
    const std::vector<Drawn>& drawn() const { return drawn_; }
    const std::vector<RangeRing>& bands() const { return bands_; }

    static constexpr u32 FRAMES_IN_FLIGHT = 2;
    static constexpr u32 MAX_BANDS = 16384; ///< fills and lines a frame

private:
    VkPipeline cast_ = VK_NULL_HANDLE;
    VkPipeline mask_ = VK_NULL_HANDLE;
    VkPipeline fill_ = VK_NULL_HANDLE;
    VkPipeline burn_ = VK_NULL_HANDLE;
    VkPipelineLayout cast_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout screen_layout_ = VK_NULL_HANDLE;
    AllocatedBuffer volume_ = {};  ///< the ring volume's vertices
    AllocatedBuffer indices_ = {}; ///< and its triangles
    u32 index_count_ = 0;
    AllocatedBuffer band_buf_[FRAMES_IN_FLIGHT] = {};
    void* band_mapped_[FRAMES_IN_FLIGHT] = {};
    f32 bottom_ = -5.0f, top_ = 256.0f;
    bool fill_on_ = false;
    std::vector<Drawn> drawn_;
    std::vector<RangeRing> bands_;
    bool warned_full_ = false;
};

} // namespace osc::renderer
