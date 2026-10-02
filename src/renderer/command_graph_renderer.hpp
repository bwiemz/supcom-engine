#pragma once

#include "core/types.hpp"
#include "renderer/vk_types.hpp"
#include "sim/entity.hpp"
#include "sim/unit_command.hpp"

#include <vulkan/vulkan.h>

#include <array>
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
class TextureCache;

/// How an order of one kind is drawn: its CommandGraphParams entry
/// (/lua/ui/game/commandgraphparams.lua), the keys Moho's UICommandGraph
/// reads. Colours are RGBA, 0 to 1.
struct CommandGraphStyle {
    std::string line_texture;
    f32 uv_aspect = 1.0f; ///< orderline_uv_aspect_ratio
    f32 anim_rate = 0.0f; ///< orderline_anim_rate: U scrolled per second
    std::array<f32, 4> line_color{};
    std::array<f32, 4> line_selected_color{};
    std::string waypoint_texture; ///< none: no waypoint drawn
    std::array<f32, 4> waypoint_color{};
    std::array<f32, 4> waypoint_selected_color{};
    f32 waypoint_scale = 1.0f;
    f32 waypoint_selected_scale = 1.0f;
};

/// The entry `name` of the CommandGraphParams table at stack index
/// `params`: each key its own, else its inherit_from entry's, else
/// `default`'s, as the table's header says.
CommandGraphStyle command_graph_style(lua_State* L, int params, const std::string& name);

/// The CommandGraphParams key of an order's kind ("UNITCOMMAND_Move"...);
/// empty for one Moho draws no line for.
std::string command_graph_key(sim::CommandType type);

/// A segment's strip on the ground, as retail lays an order's leg: its
/// corners, `half_width` either side of the segment from `a` to `b`. False
/// for a segment with no length across the ground.
bool command_strip(const sim::Vector3& a, const sim::Vector3& b, f32 half_width,
                   std::array<sim::Vector3, 4>& corners);

/// Leg `leg` of an order chain as Moho's UICommandGraph bends it, in
/// `segments` + 1 points: a cubic Hermite curve, its tangents bisecting the
/// legs at each waypoint, a quarter of the leg long, at most
/// `smoothness` × `width`.
std::vector<sim::Vector3> command_curve(const std::vector<sim::Vector3>& chain, size_t leg,
                                        u32 segments, f32 width, f32 smoothness = 50.0f);

/// Draws the selected units' order lines and waypoints as Moho's
/// UICommandGraph does, while Shift is held: each leg a textured strip on
/// the ground, in its order's colours; each order's waypoint icon lying at
/// its end.
/// primbatcher.fx's TCommand: texture × colour, alpha blended, over the
/// world (no depth test), writing no alpha (no glow).
class CommandGraphRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
              VkDescriptorSetLayout texture_ds_layout);

    /// Build this frame's strips for the units in `selected` (may be null).
    /// `L` is the UI state, whose CommandGraphParams give the styles (read
    /// once); `time` in seconds scrolls an animated line. `shown`: Shift is
    /// held.
    void update(const sim::FrameView& view, const Camera& camera,
                const std::unordered_set<u32>* selected, TextureCache& tex_cache, lua_State* L,
                f32 time, u32 viewport_h, bool shown, u32 fi);

    void render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h, const f32* view_proj,
                u32 fi) const;

    void destroy(VkDevice device, VmaAllocator allocator);

    /// A leg drawn this frame (tests read them)
    struct Leg {
        u32 unit = 0;
        sim::CommandType type = sim::CommandType::Move;
        sim::Vector3 from, to;
        std::array<f32, 4> color{};
        std::string texture;
    };
    const std::vector<Leg>& legs() const { return legs_; }

    static constexpr u32 MAX_QUADS = 32768;
    /// Segments a leg's curve is drawn in (ui_CurveSegments)
    static constexpr u32 kCurveSegments = 20;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;
    /// A leg's width in the world (CalculateWaypointLineWidth for one unit)
    static constexpr f32 kLineWidth = 1.0f;
    /// A waypoint's size in the world, and its least and most on the screen
    /// (ui_MinWaypointSize, ui_MaxWaypointSize)
    static constexpr f32 kWaypointSize = 1.5f;
    static constexpr f32 kMinWaypointPx = 7.0f;
    static constexpr f32 kMaxWaypointPx = 100.0f;

private:
    struct Vertex {
        f32 pos[3];
        f32 uv[2];
        f32 color[4];
    };
    struct Group {
        VkDescriptorSet ds = VK_NULL_HANDLE;
        u32 first_vertex = 0, vertex_count = 0;
    };
    const CommandGraphStyle* style(sim::CommandType type, lua_State* L);

    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    AllocatedBuffer vertex_buf_[FRAMES_IN_FLIGHT] = {};
    void* vertex_mapped_[FRAMES_IN_FLIGHT] = {};
    std::vector<Group> groups_;
    std::vector<Leg> legs_;
    std::unordered_map<std::string, CommandGraphStyle> styles_;
    bool styles_read_ = false;
};

} // namespace osc::renderer
