#pragma once

#include "renderer/vk_types.hpp"
#include "renderer/ui_renderer.hpp" // UIInstance, UIDrawGroup
#include "core/types.hpp"

#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct lua_State;

namespace osc::sim {
class FrameView;
struct EntityRecord;
} // namespace osc::sim

namespace osc::renderer {

class Camera;
class ReconView;
class TextureCache;
struct GPUTexture;
struct MapArea;

/// Icon types derived from unit categories.
enum class StrategicIconType : u8 {
    Land = 0,
    Air,
    Naval,
    Engineer,
    Commander,
    Structure,
    Generic,
    COUNT
};

/// Draws FA's strategic icons, as Moho's CWldSession::RenderStrategicIcons
/// (M215c): each unit's blueprint icon (StrategicIconName: its rest or
/// selected texture, at the texture's own size, tinted by its army's
/// colour) once the camera is out past its mesh's IconFadeInZoom; a blip's
/// at any zoom, generic (structure, land, naval, air) in UnidentifiedColor
/// until it's been seen. Ground icons first, then air, then high-priority
/// (StrategicIconSortPriority under 'A'), then the selected; a stunned
/// unit's badge over its icon.
class StrategicIconRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator);

    /// Build the procedural icon atlas texture (called once after TextureCache ready).
    void build_atlas(TextureCache& tex_cache);

    /// The player's intel: its blips show as icons at any zoom, and what it
    /// doesn't see not at all (null: everything seen; M215a).
    void set_recon(const ReconView* recon) { recon_ = recon; }

    /// Update icon quads from the world as `view` draws it; `L` holds the
    /// blueprints (__blueprints) and imports strategicIcons.lua. Returns true
    /// if strategic zoom is active (meshes give way to icons).
    bool update(const sim::FrameView& view, const Camera& camera,
                const std::array<f32, 16>& vp_matrix, const std::unordered_set<u32>* selected_ids,
                TextureCache& tex_cache, u32 viewport_w, u32 viewport_h, lua_State* L = nullptr);

    /// A minimap's icons, the map drawn at `area`: its own world view through
    /// the same pass (faf-re CUIWorldView::Render), fully zoomed out.
    void paint_map(const sim::FrameView& view, const MapArea& area, f32 map_w, f32 map_h,
                   const std::unordered_set<u32>* selected_ids, TextureCache& tex_cache,
                   lua_State* L, std::vector<UIQuad>& out);

    /// Forget what the last game's blueprints and strategicIcons.lua said:
    /// the next game's may differ (a scene rebuilt for it).
    f32 fade_in_zoom(const std::string& blueprint_id, lua_State* L) {
        return icon_blueprint(blueprint_id, L).fade_in_zoom;
    }

    void forget_blueprints() {
        icon_blueprints_.clear();
        underlay_textures_.clear();
        generic_loaded_ = false;
    }

    /// Load these blueprints' icons (and the generic and stunned ones) now,
    /// as Moho loads a blueprint's icons with the blueprint.
    void preload(const std::vector<std::string>& blueprint_ids, TextureCache& tex_cache,
                 lua_State* L);

    /// Issue draw calls. Caller must have the UI pipeline bound.
    void render(VkCommandBuffer cmd, VkPipelineLayout layout,
                u32 viewport_w, u32 viewport_h);

    void destroy(VkDevice device, VmaAllocator allocator);

    void set_frame_index(u32 fi) { fi_ = fi; }
    /// This frame's quads (the render-state dump reads them), and each one's
    /// texture.
    const std::vector<UIInstance>& quads() const { return quads_; }
    const std::vector<std::string>& quad_textures() const { return quad_textures_; }

    u32 quad_count() const { return quad_count_; }
    bool is_strategic_zoom() const { return strategic_zoom_active_; }
    /// ui_AlwaysRenderStrategicIcons (M217i): icons at every zoom, over the
    /// meshes, past no IconFadeInZoom.
    void set_always(bool on) { always_ = on; }
    bool always() const { return always_; }
    /// ui_NisRenderIcons ("nis toggle for strat icons"): off, no icons at
    /// all, as a campaign's NIS turns them off.
    void set_nis_icons(bool on) { nis_icons_ = on; }
    bool nis_icons() const { return nis_icons_; }
    VkDescriptorSet atlas_descriptor() const { return atlas_ds_; }

    /// Camera distance past which meshes give way to icons altogether.
    static constexpr f32 ZOOM_THRESHOLD = 250.0f;
    /// Where FA's strategic icon textures live (REntityBlueprint).
    static constexpr const char* kIconDirectory = "/textures/ui/common/game/strategicicons/";
    static constexpr u32 MAX_ICON_QUADS = 4096;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

    /// Classify a unit into an icon type based on its categories.
    static StrategicIconType classify_unit(const sim::EntityRecord& unit);

    /// A never-seen blip's icon: a structure's, or by what it moves on (air,
    /// naval, else land), as Moho's generic blip icons.
    static StrategicIconType classify_blip(const sim::EntityRecord& unit);

    /// Atlas layout constants (public for reuse by SelectionInfoRenderer).
    static constexpr u32 ICON_CELL_SIZE = 32;
    static constexpr u32 ATLAS_COLS = static_cast<u32>(StrategicIconType::COUNT);
    static constexpr u32 ATLAS_W = ATLAS_COLS * ICON_CELL_SIZE; // 7 * 32 = 224
    static constexpr u32 ATLAS_H = ICON_CELL_SIZE;              // single row

private:
    /// Project world position to screen pixel coordinates.
    static bool world_to_screen(f32 wx, f32 wy, f32 wz, const std::array<f32, 16>& vp, f32 sw,
                                f32 sh, f32& out_x, f32& out_y);

    /// A texture's quad centred at (x, y), at its own size, tinted.
    static UIInstance icon_quad(f32 x, f32 y, const GPUTexture& tex, f32 r, f32 g, f32 b);
    void emit_icon(f32 x, f32 y, const std::string& path, const GPUTexture& tex, f32 r, f32 g,
                   f32 b);

    struct Icon {
        f32 x = 0, y = 0;
        const std::string* path = nullptr;
        const GPUTexture* tex = nullptr;
        f32 r = 1, g = 1, b = 1;
        bool stunned = false;
        const std::string* underlay_path = nullptr;
        const GPUTexture* underlay = nullptr;
    };
    /// Moho's four runs, drawn in this order: ground, air, high-priority,
    /// selected.
    using Runs = std::array<std::vector<Icon>, 4>;
    template <class Place>
    Runs collect(const sim::FrameView& view, const std::unordered_set<u32>* selected_ids,
                 TextureCache& tex_cache, lua_State* L, bool fade, f32 cam_dist, f32 fade_cap,
                 Place&& place);
    template <class Emit> void emit_runs(const Runs& runs, TextureCache& tex_cache, Emit&& emit);

    /// What a blueprint's icon draws with (Moho's REntityBlueprint fields).
    struct IconBlueprint {
        std::string rest, selected; ///< its textures; empty: no icon
        u8 sort_priority = 0;       ///< StrategicIconSortPriority, a byte
        bool can_fly = false;       ///< Air.CanFly: the air run
        f32 fade_in_zoom = 0;       ///< its mesh's IconFadeInZoom
    };
    const IconBlueprint& icon_blueprint(const std::string& id, lua_State* L);
    /// An underlay's texture (Unit:SetStrategicUnderlay's name, as a
    /// strategic icon's: under the icons' directory unless absolute).
    const std::string& underlay_texture(const std::string& name);
    /// strategicIcons.lua's GenericIcons and StunnedIcons, read once.
    void load_generic_icons(lua_State* L);

    /// Generate a single icon shape into pixel buffer.
    static void draw_icon_shape(u8* pixels, u32 atlas_w,
                                u32 cell_x, u32 cell_y, u32 cell_size,
                                StrategicIconType type);

    AllocatedBuffer instance_buf_[FRAMES_IN_FLIGHT] = {};
    void* instance_mapped_[FRAMES_IN_FLIGHT] = {};
    u32 fi_ = 0;

    std::vector<UIInstance> quads_;
    std::vector<std::string> quad_textures_;
    u32 quad_count_ = 0;
    /// Runs of quads that share a texture, in draw order.
    struct Group {
        VkDescriptorSet ds = VK_NULL_HANDLE;
        u32 first = 0, count = 0;
    };
    std::vector<Group> groups_;

    std::unordered_map<std::string, IconBlueprint> icon_blueprints_;
    std::unordered_map<std::string, std::string> underlay_textures_;
    bool generic_loaded_ = false;
    std::string generic_structure_, generic_land_, generic_naval_, generic_air_, stunned_;

    VkDescriptorSet atlas_ds_ = VK_NULL_HANDLE;
    bool strategic_zoom_active_ = false;
    bool nis_icons_ = true;
    bool always_ = false;
    const ReconView* recon_ = nullptr;
};

} // namespace osc::renderer
