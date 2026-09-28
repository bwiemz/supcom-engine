#pragma once

#include "core/types.hpp"
#include "renderer/vk_types.hpp"
#include "sim/entity.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct lua_State;

namespace osc::sim {
class FrameView;
}

namespace osc::renderer {

class Camera;
class Frustum;
class ReconView;
class TextureCache;
struct TrailBlueprintData;
class TrailBlueprintCache;

/// Draws FA's trails (M214b), as Moho's CEfxTrailEmitter and particle.fx's
/// TrailVS/TrailPS do. Each tick a trail emits a segment from where its
/// point was to where it is (only while the player could see it, catching
/// up when it can again). The segments then live on their own for the
/// blueprint's TrailLength ticks, drawn as a ribbon facing the camera: the
/// ramp texture by age, the repeat texture by distance travelled, blended
/// by a TPolyTrail technique.
class TrailRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
              VkDescriptorSetLayout texture_ds_layout);

    /// Emit the segments of a tick `view` hasn't shown before, then build
    /// this frame's ribbons. What the player's intel doesn't show
    /// (`recon`, may be null) doesn't emit; nor does what is out of view
    /// (`frustum`, may be null) or past its LODCutoff.
    void update(const sim::FrameView& view, const Camera& camera, const Frustum* frustum,
                TrailBlueprintCache& blueprints, TextureCache& tex_cache, lua_State* L,
                const ReconView* recon, u32 fi);

    /// Draw the trails under the water (negative SortOrder) or the others;
    /// the scene pass must be open.
    void render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h, const f32* view_proj,
                bool under_water, u32 fi) const;

    /// Forget every trail and segment (a new game).
    void clear();

    void destroy(VkDevice device, VmaAllocator allocator);

    /// A segment drawn this frame (the render-state dump and tests read them).
    struct Drawn {
        u32 effect_id = 0;
        std::string blueprint;
        sim::Vector3 start, end;
        sim::Vector3 start_tangent; ///< the direction its start carries
        f32 t_start = 0, t_end = 0; ///< each end's age fraction
        f32 u_start = 0, u_end = 0; ///< each end's distance coordinate
        f32 size = 0;
        i32 blendmode = 0;
        bool under_water = false;
    };
    const std::vector<Drawn>& drawn() const { return drawn_; }

    /// Whether effect `id` is a trail it draws (one with a blueprint): the
    /// overlay leaves those to it.
    bool draws_effect(u32 id) const { return emitters_.count(id) > 0; }

    static constexpr u32 MAX_SEGMENTS = 16384;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;
    /// Most missed segments a trail catches up (CEfxTrailEmitter::OnTick).
    static constexpr u32 MAX_CATCHUP = 24;

private:
    struct Point {
        sim::Vector3 pos;
        u32 tick = 0;
    };
    /// One trail effect's emitter (CEfxTrailEmitter).
    struct Emitter {
        const TrailBlueprintData* bp = nullptr;
        const std::string* blueprint = nullptr;
        std::deque<Point> points;      ///< its last points, newest last
        u32 first_tick = 0;            ///< when it appeared
        u32 missed = 0;                ///< ticks it didn't emit (mTrailLength)
        bool created = false;          ///< it has emitted (so has a last direction)
        sim::Vector3 last_dir;         ///< its last segment's direction
        f32 travelled = 0;             ///< mLength
        std::optional<u32> first_look; ///< mLastUpdate: its intel looks every 5th tick from here
        bool seen = false;             ///< mVisible: what the last look found
    };
    struct Segment {
        u32 effect_id = 0;
        const TrailBlueprintData* bp = nullptr;
        const std::string* blueprint = nullptr;
        sim::Vector3 start, end, start_tangent, end_tangent;
        u32 start_born = 0, end_born = 0; ///< render-clock ticks each end appears at
        f32 u_start = 0, u_end = 0;
    };
    struct Vertex {
        f32 pos[3];
        f32 tvu[3];
    };
    /// A run of ribbons that share a pass, a blend and textures.
    struct Group {
        bool under_water = false;
        i32 blendmode = 0;
        VkDescriptorSet ramp = VK_NULL_HANDLE, repeat = VK_NULL_HANDLE;
        u32 first_vertex = 0, vertex_count = 0;
    };

    /// Record the tick `view` shows and emit what it brings (OnTick).
    void advance(const sim::FrameView& view, const sim::Vector3& eye, const Frustum* frustum,
                 TrailBlueprintCache& blueprints, lua_State* L, const ReconView* recon);
    /// Whether `e` emits this tick (CalculateVisible / CanSeeCam).
    bool can_emit(Emitter& e, const sim::FrameView& view, u32 tick, const sim::Vector3& eye,
                  const Frustum* frustum, const ReconView* recon) const;
    /// Emit `e`'s segment from `from` to `to` (Tick).
    void emit(u32 id, Emitter& e, const Point& from, const Point& to);

    std::array<VkPipeline, 5> pipelines_{};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    AllocatedBuffer vertex_buf_[FRAMES_IN_FLIGHT] = {};
    void* vertex_mapped_[FRAMES_IN_FLIGHT] = {};

    std::unordered_map<u32, Emitter> emitters_;
    std::unordered_set<u32> unknown_;       ///< trail effects without a readable blueprint
    std::unordered_set<std::string> names_; ///< blueprint paths the segments point at
    std::vector<Segment> segments_;
    std::optional<u32> last_tick_;
    std::vector<Group> groups_;
    std::vector<Drawn> drawn_;
};

} // namespace osc::renderer
