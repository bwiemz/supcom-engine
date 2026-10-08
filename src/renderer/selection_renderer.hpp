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

/// Moho's selection indicators (ren_SelectBoxes): brackets round the
/// selected units and the one under the cursor, and the drag-selection box
/// on the ground.
class SelectionRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
              VkDescriptorSetLayout texture_ds_layout);

    /// This frame's brackets for `selected` and `hovered` (0: none), and the
    /// drag box `drag` (its four corners on the ground). `L` is the UI state,
    /// whose blueprints give the boxes.
    void update(const sim::FrameView& view, const Camera& camera, u32 viewport_h,
                const std::unordered_set<u32>* selected, u32 hovered, i32 player_army,
                const std::optional<std::array<sim::Vector3, 4>>& drag, TextureCache& tex_cache,
                MeshCache& mesh_cache, lua_State* L, u32 fi);

    void render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h, const f32* view_proj,
                u32 fi) const;

    void destroy(VkDevice device, VmaAllocator allocator);

    void set_enabled(bool on) { enabled_ = on; }

    /// A unit's brackets drawn this frame (tests read them)
    struct Bracket {
        u32 unit = 0;
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
    bool enabled_ = true;
    std::vector<Bracket> brackets_;
    bool drew_drag_box_ = false;
    std::unordered_map<std::string, SelectionBlueprint> box_blueprints_;
};

} // namespace osc::renderer
