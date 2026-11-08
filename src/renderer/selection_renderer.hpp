#pragma once

#include "core/types.hpp"
#include "renderer/world_quad_batch.hpp"
#include "sim/entity.hpp"

#include <array>
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
class MeshCache;
class ReconView;
class TextureCache;

/// A unit's selection box, flattened to its four ground corners
struct SelectionBox {
    sim::Vector3 center;
    sim::Vector3 right;   ///< the unit's X axis, unit length
    sim::Vector3 forward; ///< its Z axis
    f32 half_x = 0, half_z = 0;
};

struct SelectionBlueprint {
    f32 size_x = 0, size_z = 0;
    sim::Vector3 offset;
    f32 thickness = 0;
};

/// The box Moho brackets (func_DrawSelectionBrackets): `mesh_min`/`mesh_max`
/// are the unit's LOD 0 mesh bounds, scaled
SelectionBox selection_box(const SelectionBlueprint& bp, const sim::Vector3& mesh_min,
                           const sim::Vector3& mesh_max, const sim::Vector3& position,
                           const sim::Quaternion& orientation);

/// Half the side of a bracket tile in world units: `thickness` of the box's
/// longer half, but at least `min_px` pixels of `px_world` each
f32 bracket_corner(const SelectionBox& box, f32 thickness, f32 min_px, f32 px_world);

/// The four bracket tiles centred on the box's corners (back-left, back-right,
/// front-right, front-left), each from its own back-left corner
std::array<std::array<sim::Vector3, 4>, 4> bracket_quads(const SelectionBox& box, f32 corner);

/// Moho's UserEntity::GetSelectionBracketTexture for the player of army
/// `viewer` (-1: an observer); null for an enemy's unit never in its sight.
const char* bracket_texture(i32 army, i32 viewer, u32 allies, u32 enemies, bool seen_ever);

/// A unit's brackets flashing on and off for a while (Moho's BlinkyBox,
/// pushed by AddBlinkyBox(entityId, onTime, offTime, totalTime)).
struct BlinkyBox {
    sim::EntityId unit{};
    bool on = false;  ///< it starts off
    f32 duration = 0; ///< seconds since it was given
    f32 cycle = 0;    ///< seconds into its current half
    f32 on_time = 0;
    f32 off_time = 0;
    f32 total_time = 0;
};

/// Run a blinky box on by `dt` seconds (func_RenUI 0x007FD8B7): it changes
/// half when the time in its half passes that half's length, the overshoot
/// carried into the next. Whether it is lit after.
bool advance_blinky_box(BlinkyBox& box, f32 dt);

/// Moho's selection indicators (ren_SelectBoxes): brackets round the
/// selected units and the one under the cursor, and the drag-selection box
/// on the ground.
class SelectionRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
              VkDescriptorSetLayout texture_ds_layout);

    /// This frame's brackets for `selected` and `hovered` (0: none), the
    /// blinky boxes lit after `dt` seconds more, and the drag box `drag` (its
    /// four corners on the ground). `L` is the UI state, whose blueprints
    /// give the boxes.
    void update(const sim::FrameView& view, const Camera& camera, u32 viewport_h,
                const std::unordered_set<sim::EntityId>* selected, sim::EntityId hovered,
                i32 player_army, const ReconView& recon,
                const std::optional<std::array<sim::Vector3, 4>>& drag, TextureCache& tex_cache,
                MeshCache& mesh_cache, lua_State* L, u32 fi, f32 dt);

    /// AddBlinkyBox: `unit`'s brackets, in its army's colour for the player,
    /// flash on for `on_time` seconds and off for `off_time`, from off, until
    /// `total_time` seconds have passed or the unit is gone. Kept as Moho's
    /// are, after the boxes given before.
    void add_blinky_box(sim::EntityId unit, f32 on_time, f32 off_time, f32 total_time);
    const std::vector<BlinkyBox>& blinky_boxes() const { return blinky_boxes_; }
    /// The world is gone, and its units with it (Moho's weak links die).
    void clear_blinky_boxes() { blinky_boxes_.clear(); }

    /// The blinky boxes' turn of a frame: those whose unit `exists` no more,
    /// or whose time is up, go; the rest run on by `dt`. The units of those
    /// lit, in their order.
    template <typename Exists>
    std::vector<sim::EntityId> tick_blinky_boxes(f32 dt, Exists&& exists);

    void render(VkCommandBuffer cmd, const VkRect2D& area, const f32* view_proj, u32 fi) const;

    void destroy(VkDevice device, VmaAllocator allocator);

    void set_enabled(bool on) { enabled_ = on; }

    /// A unit's brackets drawn this frame (tests read them)
    struct Bracket {
        sim::EntityId unit{};
        std::string texture;
        SelectionBox box;
        f32 corner = 0; ///< half a tile's side, world units
    };
    const std::vector<Bracket>& brackets() const { return brackets_; }
    bool drew_drag_box() const { return drew_drag_box_; }

    static constexpr u32 MAX_QUADS = 8192;

private:
    const SelectionBlueprint& box_blueprint(const std::string& id, lua_State* L);

    WorldQuadBatch batch_;
    WorldQuadBatch drag_batch_;
    bool enabled_ = true;
    std::vector<Bracket> brackets_;
    bool drew_drag_box_ = false;
    std::unordered_map<std::string, SelectionBlueprint> box_blueprints_;
    std::vector<BlinkyBox> blinky_boxes_;
};

template <typename Exists>
std::vector<sim::EntityId> SelectionRenderer::tick_blinky_boxes(f32 dt, Exists&& exists) {
    std::vector<sim::EntityId> lit;
    size_t kept = 0;
    for (BlinkyBox& box : blinky_boxes_) {
        if (!exists(box) || box.duration > box.total_time) {
            continue;
        }
        if (advance_blinky_box(box, dt)) {
            lit.push_back(box.unit);
        }
        blinky_boxes_[kept++] = box;
    }
    blinky_boxes_.resize(kept);
    return lit;
}

} // namespace osc::renderer
