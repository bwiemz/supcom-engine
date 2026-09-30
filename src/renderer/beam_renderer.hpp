#pragma once

#include "core/types.hpp"
#include "renderer/vk_types.hpp"
#include "sim/entity.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <string>
#include <unordered_set>
#include <vector>

struct lua_State;

namespace osc::sim {
class FrameView;
}

namespace osc::renderer {

class BeamBlueprintCache;
class Camera;
class ReconView;
class TextureCache;

/// Draws FA's beams (M214a): each beam effect with a BeamBlueprint as a
/// textured strip facing the camera, as particle.fx's BeamVS/BeamPS and
/// Moho's EmitInterpolatedBeamQuadVertices draw it, blended by its
/// blueprint's Blendmode (a pipeline per TBeam technique).
class BeamRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
              VkDescriptorSetLayout texture_ds_layout);

    /// Build this frame's strips from the world as `view` draws it. `time`
    /// is FA's (ticks, with the frame's interpolant); beams the player's
    /// intel doesn't show (`recon`, may be null) aren't drawn.
    void update(const sim::FrameView& view, const Camera& camera, BeamBlueprintCache& blueprints,
                TextureCache& tex_cache, lua_State* L, const ReconView* recon, f32 time, u32 fi);

    /// Draw them; the scene pass must be open.
    void render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h, const f32* view_proj,
                u32 fi) const;

    void destroy(VkDevice device, VmaAllocator allocator);

    /// A beam drawn this frame (the render-state dump and tests read them).
    struct Drawn {
        u32 effect_id = 0;
        std::string blueprint;
        sim::Vector3 start, end;
        f32 thickness = 0;
        std::array<f32, 4> start_color{}, end_color{};
        i32 blendmode = 0;
        f32 u_offset = 0; ///< UShift·time
        f32 v_start = 0;  ///< V at the start (VShift·time)
        f32 v_end = 0;    ///< and at the end (+ the repeat)
    };
    const std::vector<Drawn>& drawn() const { return drawn_; }

    /// Whether it drew effect `id`, or a beam on entity `id` (a collision
    /// beam's), or left it out at this fidelity: the overlay leaves those to
    /// it.
    bool drew_effect(u32 id) const { return drawn_effects_.count(id) > 0; }
    bool drew_on_entity(u32 id) const { return drawn_entities_.count(id) > 0; }
    /// graphics_Fidelity (0 low, 1 medium, 2 high). Beams are drawn afresh
    /// each frame, so one its blueprint leaves out is left out while the
    /// fidelity does (Moho decides as it makes the beam).
    void set_fidelity(int fidelity) { fidelity_ = fidelity; }

    static constexpr u32 MAX_BEAMS = 4096;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

private:
    struct Vertex {
        f32 pos[3];
        f32 uv[2];
        f32 color[4];
    };
    /// A run of strips that share a blend and a texture.
    struct Group {
        i32 blendmode = 0;
        VkDescriptorSet ds = VK_NULL_HANDLE;
        u32 first_vertex = 0, vertex_count = 0;
    };

    std::array<VkPipeline, 5> pipelines_{};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    AllocatedBuffer vertex_buf_[FRAMES_IN_FLIGHT] = {};
    void* vertex_mapped_[FRAMES_IN_FLIGHT] = {};
    std::vector<Group> groups_;
    std::vector<Drawn> drawn_;
    std::unordered_set<u32> drawn_effects_, drawn_entities_;
    int fidelity_ = 2;
};

} // namespace osc::renderer
