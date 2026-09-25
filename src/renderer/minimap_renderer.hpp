#pragma once

#include "renderer/vk_types.hpp"
#include "renderer/ui_renderer.hpp" // UIInstance
#include "core/types.hpp"

#include <array>
#include <unordered_set>
#include <vector>

namespace osc::map {
class Terrain;
}

namespace osc::sim {
class FrameView;
}

namespace osc::renderer {

class Camera;
class TextureCache;

/// Screen rect of the drawn map, in pixels.
struct MapArea {
    f32 x = 0, y = 0, w = 0, h = 0;
};

/// The largest rect with the map's aspect ratio centred in (x, y, w, h):
/// where a minimap view shows a map_w x map_h map.
MapArea fit_map_area(f32 x, f32 y, f32 w, f32 h, f32 map_w, f32 map_h);

/// The world point under screen point (mx, my) on a minimap view `view`
/// showing the map at `area`: false outside the view. A point in the
/// view's letterbox margin is still the minimap's (it must not click
/// through to the world); it maps to the nearest map edge.
bool minimap_to_world(const MapArea& view, const MapArea& area, f32 mx, f32 my,
                      f32 map_w, f32 map_h, f32& out_wx, f32& out_wz);

/// Renders the minimap: terrain, unit dots and the camera's view. The C++
/// HUD shows it in the bottom-left corner (update + render); FA's game UI
/// shows it in its minimap WorldView, drawn with the UI (paint). Clicks
/// land on it where it was drawn last (hit_test).
class MinimapRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator);

    /// Forget the map: no terrain texture and no size, so nothing is drawn
    /// until build_terrain_texture makes the next map's (the scene is being
    /// cleared, and its texture evicted).
    void forget_terrain() {
        terrain_ds_ = VK_NULL_HANDLE;
        map_w_ = 0;
        map_h_ = 0;
    }

    /// Generate the terrain background texture from heightmap.
    /// Call once from build_scene.
    void build_terrain_texture(const map::Terrain& terrain,
                               TextureCache& tex_cache);

    /// Start a frame: until it is drawn again the minimap is off screen, so
    /// no click lands on it.
    void begin_frame() {
        view_ = {};
        area_ = {};
        quads_.clear();
        draw_groups_.clear();
        quad_count_ = 0;
    }

    /// Build the C++ HUD's corner minimap and upload it for render().
    void update(const sim::FrameView& view, const Camera& camera,
                TextureCache& tex_cache,
                const std::unordered_set<u32>* selected_ids,
                u32 viewport_w, u32 viewport_h);

    /// Draw the minimap into the view rect (x, y, w, h): its quads are
    /// appended to `out`, for the UI renderer to draw at the view's depth.
    void paint(const sim::FrameView& view, const Camera& camera,
               TextureCache& tex_cache, f32 x, f32 y, f32 w, f32 h,
               u32 viewport_w, u32 viewport_h, std::vector<UIQuad>& out);

    /// Issue draw calls. Caller must have the UI pipeline bound.
    void render(VkCommandBuffer cmd, VkPipelineLayout layout,
                u32 viewport_w, u32 viewport_h);

    void destroy(VkDevice device, VmaAllocator allocator);

    void set_frame_index(u32 fi) { fi_ = fi; }
    /// This frame's quads from update() (the legacy HUD's minimap).
    const std::vector<UIQuad>& quads() const { return quads_; }

    u32 quad_count() const { return quad_count_; }

    /// Test if screen-space point (mx, my) is on the minimap drawn this
    /// frame. If true, writes world coordinates to out_wx, out_wz.
    bool hit_test(f32 mx, f32 my, u32 viewport_w, u32 viewport_h,
                  f32 map_w, f32 map_h,
                  f32& out_wx, f32& out_wz) const;

    static constexpr u32 MINIMAP_SIZE = 200; // pixels
    static constexpr u32 MINIMAP_MARGIN = 10; // from bottom-left corner
    static constexpr u32 MINIMAP_TEX_SIZE = 256; // terrain texture resolution
    static constexpr u32 MAX_MINIMAP_QUADS = 2048;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

private:
    /// Build the map's quads into quads_ for the map drawn at area_.
    void build(const sim::FrameView& view, const Camera& camera,
               TextureCache& tex_cache, u32 viewport_w, u32 viewport_h,
               bool framed);

    void emit_quad(f32 x, f32 y, f32 w, f32 h,
                   f32 r, f32 g, f32 b, f32 a,
                   VkDescriptorSet ds = VK_NULL_HANDLE);

    AllocatedBuffer instance_buf_[FRAMES_IN_FLIGHT] = {};
    void* instance_mapped_[FRAMES_IN_FLIGHT] = {};
    u32 fi_ = 0;

    std::vector<UIQuad> quads_;
    u32 quad_count_ = 0; // quads uploaded for render()

    // Terrain background texture descriptor
    VkDescriptorSet terrain_ds_ = VK_NULL_HANDLE;
    VkDescriptorSet white_ds_ = VK_NULL_HANDLE;

    // Cached map dimensions
    f32 map_w_ = 0;
    f32 map_h_ = 0;

    MapArea view_; // the minimap's screen rect this frame (empty: not drawn)
    MapArea area_; // where in view_ the map was drawn

    struct DrawGroup {
        VkDescriptorSet ds = VK_NULL_HANDLE;
        u32 offset = 0;
        u32 count = 0;
    };
    std::vector<DrawGroup> draw_groups_;
};

} // namespace osc::renderer
