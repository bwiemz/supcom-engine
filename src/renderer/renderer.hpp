#pragma once

#include "map/heightmap.hpp"
#include "map/scmap_parser.hpp"

#include "renderer/camera.hpp"
#include "renderer/mesh_cache.hpp"
#include "renderer/terrain_mesh.hpp"
#include "renderer/texture_cache.hpp"
#include "renderer/font_cache.hpp"
#include "renderer/ui_renderer.hpp"
#include "renderer/overlay_renderer.hpp"
#include "renderer/minimap_renderer.hpp"
#include "renderer/strategic_icon_renderer.hpp"
#include "renderer/hud_renderer.hpp"
#include "renderer/profile_overlay.hpp"
#include "renderer/selection_info_renderer.hpp"
#include "ui/ui_dispatch.hpp"
#include "renderer/recon_view.hpp"
#include "renderer/unit_renderer.hpp"
#include "renderer/water_renderer.hpp"
#include "renderer/fog_renderer.hpp"
#include "renderer/particle_system.hpp"
#include "renderer/particle_renderer.hpp"
#include "renderer/beam_blueprint.hpp"
#include "renderer/beam_renderer.hpp"
#include "renderer/trail_blueprint.hpp"
#include "renderer/trail_renderer.hpp"
#include "renderer/emitter_blueprint.hpp"
#include "renderer/normal_overlay.hpp"
#include "renderer/vk_types.hpp"
#include "core/image.hpp"
#include "core/types.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <optional>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct GLFWwindow;
struct lua_State;

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::sim {
class FrameView;
struct WorldEvents;
}

namespace osc::map {
class Terrain;
struct StratumInfo;
}

namespace osc::blueprints {
class BlueprintStore;
}

namespace osc::renderer {

/// A structure being placed: drawn translucent at its snapped spot, green
/// where it can be built and red where it can't. Input works it out.
struct BuildGhost {
    std::string blueprint_id;
    f32 x = 0, y = 0, z = 0;
    bool valid = true;
};

class Renderer {
public:
    /// Initialize Vulkan, GLFW window, and all pipelines.
    /// Returns false if Vulkan is unavailable (fall back to headless).
    /// Create the window and Vulkan device.
    ///
    /// offscreen: for scripted captures (screenshots, golden images). The GLFW
    /// window is never shown -- showing blocks until the compositor maps the
    /// window, which never happens with the screen locked -- so nothing
    /// appears and focus is never taken. Rendering still targets the hidden
    /// window's surface. With OSC_HEADLESS_SURFACE=1 it targets a
    /// VK_EXT_headless_surface instead (Mesa drivers incl. lavapipe; not
    /// NVIDIA's proprietary driver), for machines without any display.
    bool init(u32 width, u32 height, const std::string& title,
              bool offscreen = false);

    /// One-time scene upload (terrain mesh, static buffers, and the meshes
    /// of `preload`, e.g. sim::world_blueprints).
    void build_scene(const map::Terrain* terrain, blueprints::BlueprintStore* store,
                     const std::vector<std::string>& preload,
                     vfs::VirtualFileSystem* vfs, lua_State* L);

    /// Tear down scene-specific GPU resources for map reload.
    void clear_scene();
    /// The UI's controls are being replaced with a new UI state's.
    void forget_ui_controls() { ui_dispatch_.forget_controls(); }

    /// Render one frame from the world as `view` draws it, between the
    /// sim's last two ticks. It shows (and takes) the death flashes and
    /// camera shakes in `events`, and draws `ghost` when placing a structure.
    /// The renderer reads no live sim state.
    void render(const sim::FrameView& view, sim::WorldEvents& events,
                const BuildGhost* ghost, lua_State* L,
                ui::UIControlRegistry* ui_registry = nullptr,
                const std::unordered_set<u32>* selected_ids = nullptr);

    /// Everything the last render() generated from the world -- mesh
    /// instances and bone poses, overlay, strategic-icon, minimap and HUD
    /// quads, emitter origins -- as sorted text lines, so two runs (or two
    /// implementations) can be compared exactly.
    void dump_frame(std::ostream& out) const;

    /// Render only the UI layer (no 3D scene, no bloom).
    /// Used during the loading screen and front end, when there is no world.
    void render_ui_only(lua_State* L, ui::UIControlRegistry* ui_registry);

    /// Initialize texture/font caches without a full scene build.
    /// Used for UI-only rendering when no map is loaded.
    void init_ui_caches(vfs::VirtualFileSystem* vfs);

    /// Access texture cache (for MapPreview uploads).
    TextureCache& texture_cache() { return texture_cache_; }

    /// Returns true if the window close was requested.
    bool should_close() const;

    /// Poll window events and update camera.
    void poll_events(f64 dt);

    /// Clean up all Vulkan resources.
    void shutdown();

    /// Mouse scroll callback (called from GLFW callback).
    void on_scroll(f64 y_offset);

    /// Check if a GLFW key is currently pressed.
    bool is_key_pressed(int glfw_key) const;

    /// The FA technique (mesh.fx) that draws a blueprint's mesh (M211b).
    MeshTechnique mesh_technique(const std::string& blueprint_id, lua_State* L);

    /// Update the window title bar text.
    void set_window_title(const char* title);

    Camera& camera() { return camera_; }
    const Camera& camera() const { return camera_; }
    /// The player's army (0-based), whose fog of war and intel the view
    /// shows; -1 for an observer, who sees everything.
    void set_player_army(i32 army) { player_army_ = army; }
    i32 player_army() const { return player_army_; }
    /// What the player's army sees of the world as of the last frame (M215a).
    const ReconView& recon() const { return recon_; }
    /// The effects' emitters and particles (tests read them).
    const ParticleSystem& particle_system() const { return particle_system_; }
    /// The beams drawn last frame (tests read them; M214a).
    const BeamRenderer& beam_renderer() const { return beam_renderer_; }
    /// The trail segments drawn last frame (tests read them; M214b).
    const TrailRenderer& trail_renderer() const { return trail_renderer_; }
    /// The map's water (tests read its water map and Fresnel table; M213a).
    const WaterRenderer& water_renderer() const { return water_renderer_; }
    /// Off, the fog of war neither dims the world nor hides what's in it.
    void set_fog_enabled(bool enabled) { fog_enabled_ = enabled; }
    bool fog_enabled() const { return fog_enabled_; }
    void set_decals_enabled(bool enabled) { decals_enabled_ = enabled; }
    bool decals_enabled() const { return decals_enabled_; }
    void set_bloom_enabled(bool b) { bloom_enabled_ = b; }
    bool bloom_enabled() const { return bloom_enabled_; }
    u32 stored_decal_count() const { return static_cast<u32>(stored_decals_.size()); }
    const MinimapRenderer& minimap() const { return minimap_renderer_; }

    /// --legacy-hud: keep drawing the engine's C++ HUD placeholders (economy
    /// bars, minimap, selection panel, game-over banner) while FA's own game
    /// interface is up. Without it they only stand in when there is none.
    void set_legacy_hud(bool enabled) { legacy_hud_ = enabled; }
    /// Whether this frame draws the C++ HUD placeholders.
    bool legacy_hud_active() const { return legacy_hud_active_; }
    u32 width() const { return window_width_; }
    u32 height() const { return window_height_; }

    /// Get current mouse position in screen pixels.
    void mouse_position(f64& x, f64& y) const;

    /// Check if a mouse button is currently pressed.
    bool is_mouse_pressed(int glfw_button) const;

    /// Receives a captured frame (RGBA8, rows top to bottom).
    using CaptureCallback = std::function<void(ImageRGBA8)>;

    /// Capture the next frame exactly as presented: after bloom, HUD and UI,
    /// from either render() or render_ui_only(). The callback runs on this
    /// thread once the GPU has finished that frame. Returns false if the
    /// swapchain cannot be read back on this driver (no TRANSFER_SRC usage).
    bool request_capture(CaptureCallback on_captured);

    /// Vulkan validation errors reported so far (0 when validation is off).
    /// Screenshot / golden runs fail if this is non-zero.
    static u32 validation_error_count() { return validation_errors_.load(); }

    /// Advance the renderer's animation clock (UI animations, water, effects)
    /// by a fixed step per frame instead of wall-clock time, so frame N looks
    /// the same on every run. 0 restores wall-clock timing.
    void set_fixed_frame_dt(f32 dt) { fixed_frame_dt_ = dt; }

    static constexpr u32 SHADOW_MAP_SIZE = 4096;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

private:
    static VkBool32 VKAPI_CALL vulkan_debug_callback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);
    static std::atomic<u32> validation_errors_;

    /// Record a copy of swapchain image `image_index` into the readback
    /// buffer (after the final render pass). Returns false if not recorded.
    bool record_capture(VkCommandBuffer cmd, u32 image_index);
    /// Wait for the GPU, convert the readback buffer to RGBA8, and hand it to
    /// the pending callback.
    void deliver_capture();

    CaptureCallback pending_capture_;
    f32 fixed_frame_dt_ = 0.0f;
    bool capture_supported_ = false;
    AllocatedBuffer capture_buf_{};
    VkDeviceSize capture_buf_size_ = 0;

    bool create_swapchain(u32 width, u32 height);
    void create_depth_image();
    void create_render_pass();
    void create_framebuffers();
    void create_pipelines();
    void recreate_swapchain();
    void create_shadow_resources();
    void create_shadow_pipelines();
    std::array<f32, 16> compute_light_vp() const;

    // GLFW
    GLFWwindow* window_ = nullptr;
    u32 window_width_ = 0;
    u32 window_height_ = 0;

    // Vulkan core
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    u32 graphics_queue_family_ = 0;
    VmaAllocator allocator_ = VK_NULL_HANDLE;

    // Swapchain
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;

    // Depth
    AllocatedImage depth_image_{};
    VkFormat depth_format_ = VK_FORMAT_D32_SFLOAT;

    // Render pass & framebuffers
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;

    // Command pool & per-frame command buffers
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_buf_[FRAMES_IN_FLIGHT] = {};

    // Per-frame sync objects
    VkFence render_fence_[FRAMES_IN_FLIGHT] = {};
    VkSemaphore present_semaphore_[FRAMES_IN_FLIGHT] = {};
    /// Render-finished semaphores, one per SWAPCHAIN IMAGE (not per frame in
    /// flight): presentation holds a semaphore until that image is presented
    /// again, so a per-frame one could be re-signalled while still pending.
    std::vector<VkSemaphore> render_finished_;

    // Pipelines
    VkPipeline terrain_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout terrain_layout_ = VK_NULL_HANDLE;
    VkPipeline unit_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout unit_layout_ = VK_NULL_HANDLE;
    VkPipeline mesh_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout mesh_layout_ = VK_NULL_HANDLE;
    VkPipeline mesh_fade_pipeline_ = VK_NULL_HANDLE; // fading instances (M211e)
    VkPipelineLayout mesh_fade_layout_ = VK_NULL_HANDLE;
    /// The build techniques' overlays that write alpha: colour and alpha
    /// blended by the source's alpha (M211f).
    VkPipeline mesh_overlay_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout mesh_overlay_layout_ = VK_NULL_HANDLE;
    /// UEFBuildCube: blended, colour only, depth tested but not written (M211g).
    VkPipeline mesh_cube_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout mesh_cube_layout_ = VK_NULL_HANDLE;
    VkPipeline decal_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout decal_layout_ = VK_NULL_HANDLE;

    // Texture infrastructure
    VkDescriptorSetLayout texture_ds_layout_ = VK_NULL_HANDLE;
    VkSampler texture_sampler_ = VK_NULL_HANDLE;

    // Bone SSBO infrastructure (set=1 for mesh pipeline, per-frame)
    VkDescriptorSetLayout bone_ds_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool bone_ds_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet bone_ds_[FRAMES_IN_FLIGHT] = {};

    // Terrain texture infrastructure (set=0 for terrain pipeline: 11 samplers)
    VkDescriptorSetLayout terrain_tex_ds_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool terrain_tex_ds_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet terrain_tex_ds_ = VK_NULL_HANDLE;
    f32 terrain_map_width_ = 0;
    f32 terrain_map_height_ = 0;
    /// The terrain shader's TerrainStrata block (std140, binding 23): each
    /// stratum's size, albedo (0-8, then the upper stratum) and normal (0-8).
    struct TerrainStrataData {
        f32 albedo_size[12]; ///< 0-8, 9: the upper stratum, 10-11: padding
        f32 normal_size[12]; ///< 0-8, 9-11: padding
    };
    static_assert(sizeof(TerrainStrataData) == 96, "six vec4s, as the shader's block");
    static constexpr u32 kTerrainStrataBinding = 23;
    AllocatedBuffer terrain_strata_ubo_{};
    /// Made per scene, with the terrain's descriptor set.
    void create_terrain_strata_ubo(const std::vector<map::StratumInfo>& strata);
    void destroy_terrain_strata_ubo();

    // Sub-renderers
    TerrainMesh terrain_mesh_;
    UnitRenderer unit_renderer_;
    WaterRenderer water_renderer_;
    FogRenderer fog_renderer_;
    UIRenderer ui_renderer_;
    OverlayRenderer overlay_renderer_;
    MinimapRenderer minimap_renderer_;
    std::vector<UIQuad> painted_minimap_; // FA minimap window's quads this frame (dump)
    StrategicIconRenderer strategic_icon_renderer_;
    HudRenderer hud_renderer_;
    SelectionInfoRenderer selection_info_renderer_;
    ProfileOverlay profile_overlay_;
    ui::UIDispatch ui_dispatch_;
    f64 last_frame_time_ = 0.0;
    f32 total_time_ = 0.0f;
    f32 frame_dt_ = 0.0f;
    MeshCache mesh_cache_;
    TextureCache texture_cache_;
    FontCache font_cache_;
    Camera camera_;
    i32 player_army_ = 0;
    ReconView recon_;
    bool fog_enabled_ = true;
    bool decals_enabled_ = true;
    bool b_key_was_pressed_ = false;

    // Decal rendering
    AllocatedBuffer decal_quad_verts_{};
    AllocatedBuffer decal_quad_indices_{};
    AllocatedBuffer decal_instance_buf_[FRAMES_IN_FLIGHT] = {};
    void* decal_instance_mapped_[FRAMES_IN_FLIGHT] = {};
    /// Free the decal quad and instance buffers (build_scene makes them for a
    /// map with decals). The device must be idle.
    void destroy_decal_buffers();

    struct StoredDecal {
        std::string texture_path;
        f32 model[16];
        f32 position_x, position_y, position_z;
        f32 cut_off_lod;
    };
    std::vector<StoredDecal> stored_decals_;

    struct DecalDrawGroup {
        VkDescriptorSet texture_ds = VK_NULL_HANDLE;
        u32 instance_offset = 0;
        u32 instance_count = 0;
    };
    std::vector<DecalDrawGroup> decal_groups_;

    static constexpr u32 MAX_DECALS = 4096;

    // UI 2D pipeline
    VkPipeline ui_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout ui_layout_ = VK_NULL_HANDLE;

    // Shadow mapping
    AllocatedImage shadow_image_{};
    VkSampler shadow_sampler_ = VK_NULL_HANDLE;
    VkRenderPass shadow_render_pass_ = VK_NULL_HANDLE;
    VkFramebuffer shadow_framebuffer_ = VK_NULL_HANDLE;

    VkPipeline shadow_terrain_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadow_terrain_layout_ = VK_NULL_HANDLE;
    VkPipeline shadow_mesh_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadow_mesh_layout_ = VK_NULL_HANDLE;
    VkPipeline shadow_unit_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadow_unit_layout_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout shadow_ds_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool shadow_ds_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet shadow_ds_[FRAMES_IN_FLIGHT] = {};

    AllocatedBuffer light_ubo_[FRAMES_IN_FLIGHT] = {};
    void* light_ubo_mapped_[FRAMES_IN_FLIGHT] = {};
    /// The lit shaders' LightUBO (std140): the shadow matrix, then the map's
    /// lighting as FA's shaders read it (M210a).
    struct LightUboData {
        f32 light_vp[16];
        f32 sun_direction[4]; ///< xyz toward the sun
        f32 sun_color[4];     ///< rgb; w: LightingMultiplier
        f32 sun_ambience[4];  ///< rgb; w: 1 for the TTerrainXP terrain shader
        f32 shadow_fill[4];   ///< rgb
        f32 specular[4];      ///< SpecularColor
    };
    /// The scene's ground, for the camera's focus (M217a).
    std::optional<map::Heightmap> ground_;
    /// Set the camera's focus height from the ground under its target.
    void update_camera_focus();
    /// Bind the map's environment cubes and FA's lookups for meshes (M211a/b).
    void bind_mesh_environment(const map::ScmapEnvironment& environment);
    /// Clamped, for FA's lookup textures.
    VkSampler lookup_sampler_ = VK_NULL_HANDLE;
    f32 ground_water_ = 0.0f;
    bool ground_has_water_ = false;
    /// The scene's lighting: its map's, else SCMP_009's.
    map::ScmapLighting lighting_{};
    bool terrain_xp_ = false;
    /// Write the lighting into every frame's UBO.
    void upload_lighting();

    // Particle system
    ParticleSystem particle_system_;
    ParticleRenderer particle_renderer_;
    EmitterBlueprintCache emitter_bp_cache_;
    /// The map build_scene drew (its water, for particles; M214c).
    const map::Terrain* terrain_ = nullptr;
    BeamRenderer beam_renderer_;
    BeamBlueprintCache beam_bp_cache_;
    TrailRenderer trail_renderer_;
    TrailBlueprintCache trail_bp_cache_;

    // Bloom post-processing
    bool bloom_enabled_ = true;
    bool legacy_hud_ = false;        // --legacy-hud
    bool legacy_hud_active_ = true;  // this frame (no FA game UI, or legacy_hud_)

    // Offscreen scene image (rendered instead of swapchain, then composited)
    /// The frame before the water, which the water refracts (M213a).
    AllocatedImage refraction_image_{};
    /// The units reflected in the water, drawn mirrored before the scene
    /// (M213b), and its framebuffer, which shares the scene's depth.
    AllocatedImage reflection_image_{};
    VkFramebuffer reflection_framebuffer_ = VK_NULL_HANDLE;
    /// The scene's two passes around the water on a map with it (M213a).
    VkRenderPass scene_first_pass_ = VK_NULL_HANDLE;
    VkRenderPass scene_second_pass_ = VK_NULL_HANDLE;
    AllocatedImage scene_color_image_{};
    VkRenderPass scene_render_pass_ = VK_NULL_HANDLE;
    VkFramebuffer scene_framebuffer_ = VK_NULL_HANDLE;

    // Bloom intermediate images (half resolution)
    AllocatedImage bloom_bright_image_{};
    AllocatedImage bloom_blur_h_image_{};
    AllocatedImage bloom_blur_v_image_{};

    VkRenderPass bloom_render_pass_ = VK_NULL_HANDLE;  // single-color-attachment pass
    VkFramebuffer bloom_bright_fb_ = VK_NULL_HANDLE;
    VkFramebuffer bloom_blur_h_fb_ = VK_NULL_HANDLE;
    VkFramebuffer bloom_blur_v_fb_ = VK_NULL_HANDLE;

    // Bloom pipelines (will be created in Task 9, declare here for cleanup)
    VkPipeline bloom_bright_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout bloom_bright_layout_ = VK_NULL_HANDLE;
    VkPipeline bloom_blur_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout bloom_blur_layout_ = VK_NULL_HANDLE;
    VkPipeline bloom_composite_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout bloom_composite_layout_ = VK_NULL_HANDLE;

    // Bloom descriptors
    VkDescriptorPool bloom_ds_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet scene_ds_ = VK_NULL_HANDLE;        // samples scene_color_image_
    VkDescriptorSet bloom_bright_ds_ = VK_NULL_HANDLE;  // samples bloom_bright_image_
    VkDescriptorSet bloom_blur_h_ds_ = VK_NULL_HANDLE;  // samples bloom_blur_h_image_
    VkDescriptorSet bloom_blur_v_ds_ = VK_NULL_HANDLE;  // samples bloom_blur_v_image_

    void create_bloom_resources();
    /// Copy the frame drawn so far for the water to refract, between the
    /// scene's two passes (M213a).
    void copy_refraction(VkCommandBuffer cmd);
    /// Which meshes a draw_meshes call draws (M213b): Moho's buckets
    /// before and after the water, or the units in its reflection.
    enum class MeshPass { All, BeforeWater, AfterWater, Reflection };
    /// The mesh instances' draws, with the scene's pipelines, seen by `vp`
    /// (for the reflection, already mirrored).
    void draw_meshes(VkCommandBuffer cmd, u32 fi, const std::array<f32, 16>& vp, MeshPass stage);
    void create_bloom_pipelines();
    void destroy_bloom_resources();

    // Frame-in-flight tracking
    u32 frame_index_ = 0;

    // Cleanup
    DeletionQueue deletion_queue_;
    bool initialized_ = false;
    bool caches_initialized_ = false;
};

} // namespace osc::renderer
