#pragma once

#include "core/types.hpp"
#include "renderer/vk_types.hpp"
#include "renderer/world_quad_batch.hpp"
#include "sim/entity.hpp"
#include "sim/unit_command.hpp"
#include "sim/world_snapshot.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct lua_State;

namespace osc::sim {
class FrameView;
struct WorldSnapshot;
struct EntityRecord;
struct CommandRecord;
} // namespace osc::sim

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
    std::array<f32, 4> waypoint_highlight_color{};
    f32 waypoint_highlight_scale = 1.0f;
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

/// A structure's pad from its footprint and skirt (Physics.SkirtSizeX/Z,
/// SkirtOffsetX/Z), centred at (x, z): {x0, z0, x1, z1}
std::array<f32, 4> build_pad(f32 x, f32 z, f32 size_x, f32 size_z, f32 skirt_x, f32 skirt_z,
                             f32 off_x, f32 off_z);

/// The units whose orders the command graph shows, each with whether it is
/// selected: the selected ones, and the army's others
std::vector<std::pair<const sim::EntityRecord*, bool>>
command_graph_units(const sim::WorldSnapshot& world, const std::unordered_set<u32>* selected,
                    i32 player_army);

struct PlannedSite {
    std::string blueprint;
    sim::Vector3 position;
};

/// Whether the structure of `builder`'s `index`-th order stands: its builder
/// is at it, or one of the army's is on the site
bool build_started(const sim::WorldSnapshot& world, const sim::EntityRecord& builder, size_t index,
                   const sim::CommandRecord& order);

std::vector<PlannedSite> planned_build_sites(const sim::WorldSnapshot& world,
                                             const std::unordered_set<u32>* selected,
                                             i32 player_army);

/// A unit's orders as the graph draws them: the places from the unit through
/// each order's target, and each leg's order and colours
struct CommandGraphPath {
    struct Leg {
        sim::CommandRecord order;
        size_t index = 0; ///< in the unit's orders, then its rally orders
        const CommandGraphStyle* style = nullptr;
        std::array<f32, 4> line_color{};
        bool closes = false; ///< the leg back to the loop's first order
    };
    const sim::EntityRecord* unit = nullptr;
    bool chosen = false;
    std::vector<sim::Vector3> chain;
    std::vector<Leg> legs;
};

/// The paths of command_graph_units: a selected unit's in its style's
/// selected colours, another's in its plain ones. A path whose first patrol,
/// guard or ground attack is not its last runs back to it, as Moho's
/// AddCommandQueueToCommandGraph links the loop.
std::vector<CommandGraphPath>
command_graph_paths(const sim::FrameView& view, const std::unordered_set<u32>* selected,
                    i32 player_army,
                    const std::function<const CommandGraphStyle*(sim::CommandType)>& style_of);

/// One waypoint for an order, shared by the units given it together (Moho's
/// UICommandGraphDrawNode)
struct CommandGraphNode {
    sim::CommandRecord order;
    const CommandGraphStyle* style = nullptr;
    sim::Vector3 position;
    f32 unit_scale = 1.0f;
    std::vector<u32> units;
    bool chosen = false;
    bool highlighted = false;
    std::array<f32, 4> color{};
    f32 scale = 1.0f;
};

/// The paths' orders, a node each: at the mean of its units' targets, as
/// big as the square root of its busier side's distinct legs
/// (RecomputeDrawNodeOrientation, ui_CommandGraphMaxNodeUnits = 1).
/// Highlighted: order `highlight`'s, or one of `hovered_unit`'s
/// (ResolveDrawNodeHighlightState).
std::vector<CommandGraphNode> command_graph_nodes(const std::vector<CommandGraphPath>& paths,
                                                  u32 highlight = 0, u32 hovered_unit = 0);

struct WaypointOnScreen {
    u32 command_id = 0;
    f32 x = 0, y = 0;
    f32 size_px = 0;
    bool chosen = false;
};

std::vector<WaypointOnScreen> waypoints_on_screen(const std::vector<CommandGraphNode>& nodes,
                                                  const Camera& camera, f32 width, f32 height);

/// The order whose waypoint is under (mx, my), as Moho's
/// ResolveCursorHighlightCommandId picks it; 0 for none
u32 waypoint_under_cursor(const std::vector<WaypointOnScreen>& waypoints, f32 mx, f32 my);

/// Draws the army's units' order lines and waypoints as Moho's
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
                const std::unordered_set<u32>* selected, i32 player_army, TextureCache& tex_cache,
                lua_State* L, f32 time, u32 viewport_h, bool shown, u32 fi);

    void render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h, const f32* view_proj,
                u32 fi) const;

    void destroy(VkDevice device, VmaAllocator allocator);

    void set_highlight(u32 command_id, u32 hovered_unit) {
        highlight_ = command_id;
        hovered_unit_ = hovered_unit;
    }

    /// A leg drawn this frame (tests read them)
    struct Leg {
        u32 unit = 0;
        sim::CommandType type = sim::CommandType::Move;
        sim::Vector3 from, to;
        std::array<f32, 4> color{};
        std::string texture;
    };
    const std::vector<Leg>& legs() const { return legs_; }

    const std::vector<PlannedSite>& planned_sites() const { return planned_; }

    static constexpr u32 MAX_QUADS = 32768;
    /// Segments a leg's curve is drawn in (ui_CurveSegments)
    static constexpr u32 kCurveSegments = 20;
    /// A leg's width in the world (CalculateWaypointLineWidth for one unit)
    static constexpr f32 kLineWidth = 1.0f;
    /// A waypoint's size in the world, and its least and most on the screen
    /// (ui_MinWaypointSize, ui_MaxWaypointSize)
    static constexpr f32 kWaypointSize = 1.5f;
    static constexpr f32 kMinWaypointPx = 7.0f;
    static constexpr f32 kMaxWaypointPx = 100.0f;
    /// A build site's outline: its width on the screen, and colour
    static constexpr f32 kPadOutlinePx = 2.0f;
    static constexpr std::array<f32, 4> kPadOutlineColor = {0.15f, 0.45f, 0.95f, 1.0f};

private:
    const CommandGraphStyle* style(sim::CommandType type, lua_State* L);

    WorldQuadBatch batch_;
    std::vector<Leg> legs_;
    std::vector<PlannedSite> planned_;
    /// A structure blueprint's footprint and skirt: size x, z, skirt x, z,
    /// offset x, z
    const std::array<f32, 6>& pad_of(const std::string& bp, lua_State* L);

    std::unordered_map<std::string, CommandGraphStyle> styles_;
    std::unordered_map<std::string, std::array<f32, 6>> pads_;
    bool styles_read_ = false;
    u32 highlight_ = 0;
    u32 hovered_unit_ = 0;
};

} // namespace osc::renderer
