#pragma once

#include "map/heightmap.hpp"
#include "map/scmap_parser.hpp"

#include "renderer/gpu_queries.hpp"
#include "renderer/vk_cmd.hpp"
#include "renderer/camera.hpp"
#include "renderer/mesh_cache.hpp"
#include "renderer/terrain_mesh.hpp"
#include "renderer/terrain_time.hpp"
#include "renderer/texture_cache.hpp"
#include "renderer/font_cache.hpp"
#include "renderer/movie_textures.hpp"
#include "renderer/ui_renderer.hpp"
#include "renderer/overlay_renderer.hpp"
#include "renderer/minimap_renderer.hpp"
#include "renderer/strategic_icon_renderer.hpp"
#include "renderer/hud_renderer.hpp"
#include "renderer/profile_overlay.hpp"
#include "renderer/selection_info_renderer.hpp"
#include "ui/ui_dispatch.hpp"
#include "renderer/playable_rect.hpp"
#include "renderer/recon_view.hpp"
#include "renderer/unit_renderer.hpp"
#include "renderer/sky_renderer.hpp"
#include "renderer/water_renderer.hpp"
#include "renderer/fog_renderer.hpp"
#include "renderer/particle_system.hpp"
#include "renderer/wave_system.hpp"
#include "renderer/particle_renderer.hpp"
#include "renderer/decal_math.hpp"
#include "renderer/runtime_decal_renderer.hpp"
#include "renderer/beam_blueprint.hpp"
#include "renderer/beam_renderer.hpp"
#include "renderer/trail_blueprint.hpp"
#include "renderer/trail_renderer.hpp"
#include "renderer/emitter_blueprint.hpp"
#include "renderer/vk_types.hpp"
#include "core/image.hpp"
#include "core/types.hpp"

#include <algorithm>
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
    f32 pad_x0 = 0, pad_z0 = 0, pad_x1 = 0, pad_z1 = 0; ///< its skirt
    bool valid = true;
    std::vector<BuildGhost> line; ///< a build drag's other sites
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
    void forget_ui_controls() {
        ui_dispatch_.forget_controls();
        movie_textures_.forget();
    }
    /// The UI's input, as the window's callbacks feed it (a scripted test
    /// clicks through it as a player would).
    ui::UIDispatch& ui_dispatch() { return ui_dispatch_; }

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
    /// The user side's playable rect, which SyncPlayableRect syncs (M209).
    UserPlayableRect& playable_rect() { return playable_rect_; }
    const UserPlayableRect& playable_rect() const { return playable_rect_; }
    /// The effects' emitters and particles (tests read them).
    const ParticleSystem& particle_system() const { return particle_system_; }
    /// The meshes this frame draws, in their groups (M211k's tests).
    const UnitRenderer& unit_renderer() const { return unit_renderer_; }
    /// Each frame slot's bone set was written with the unit renderer's SSBO
    /// for that slot as it is now (it is a new buffer once it grows).
    bool bone_sets_current() const {
        for (u32 fi = 0; fi < FRAMES_IN_FLIGHT; ++fi)
            if (bone_ds_[fi] && bone_ds_generation_[fi] != unit_renderer_.bone_ssbo_generation(fi))
                return false;
        return true;
    }
    /// The last frame's mesh draw calls in the view (the water's reflection
    /// not counted) drawn as `technique`, each pass one: Cybran's shield
    /// draws twice (M211k); a personal shield's unit counts as its base
    /// technique, its shell as its own (M211l).
    u32 mesh_draws(MeshTechnique technique) const {
        const auto i = static_cast<size_t>(technique);
        return i < mesh_draws_.size() ? mesh_draws_[i] : 0;
    }
    /// The terrain shader's Time, TTerrainGlow's scroll (tests read it;
    /// M212f).
    f32 terrain_time() const { return terrain_time_.value(); }
    /// The shoreline's wave generators (tests read them; M213c).
    const WaveSystem& wave_system() const { return wave_system_; }
    /// The beams drawn last frame (tests read them; M214a).
    const BeamRenderer& beam_renderer() const { return beam_renderer_; }
    /// The trail segments drawn last frame (tests read them; M214b).
    const TrailRenderer& trail_renderer() const { return trail_renderer_; }
    /// The map's water (tests read its water map and Fresnel table; M213a).
    const WaterRenderer& water_renderer() const { return water_renderer_; }
    const SkyRenderer& sky_renderer() const { return sky_renderer_; }
    /// Off, the fog of war neither dims the world nor hides what's in it.
    void set_fog_enabled(bool enabled) { fog_enabled_ = enabled; }
    bool fog_enabled() const { return fog_enabled_; }
    void set_decals_enabled(bool enabled) { decals_enabled_ = enabled; }
    bool decals_enabled() const { return decals_enabled_; }
    /// The session's economy overlay flag (Moho's DisplayEconomyOverlay,
    /// RenderOverlayEconomy): the MFD's economy toggle, off during a NIS.
    /// Nothing draws the overlay yet.
    void set_economy_overlay(bool on) { economy_overlay_ = on; }
    bool economy_overlay() const { return economy_overlay_; }
    void set_bloom_enabled(bool b) { bloom_enabled_ = b; }
    /// ui_AlwaysRenderStrategicIcons (M217i).
    void set_icons_always(bool on) { strategic_icon_renderer_.set_always(on); }
    bool icons_always() const { return strategic_icon_renderer_.always(); }
    /// The switches a campaign's NIS turns off (gamemain.NISMode):
    /// ui_RenderUnitBars, ui_NisRenderIcons and ren_SelectBoxes.
    void set_unit_bars(bool on) { overlay_renderer_.set_unit_bars(on); }
    bool unit_bars() const { return overlay_renderer_.unit_bars(); }
    void set_nis_icons(bool on) { strategic_icon_renderer_.set_nis_icons(on); }
    bool nis_icons() const { return strategic_icon_renderer_.nis_icons(); }
    void set_select_boxes(bool on) { overlay_renderer_.set_select_boxes(on); }
    bool select_boxes() const { return overlay_renderer_.select_boxes(); }
    /// The strategic icons drawn last frame (tests read them).
    const StrategicIconRenderer& strategic_icons() const { return strategic_icon_renderer_; }
    /// The video options (M217i): ren_Skydome, whether the sky dome draws;
    /// and those the renderer keeps but doesn't draw by yet:
    /// graphics_Fidelity, shadow_Fidelity, ren_MipSkipLevels,
    /// SC_CameraScaleLOD, SC_AntiAliasingSamples.
    struct VideoOptions {
        bool skydome = true;
        int graphics_fidelity = 2;
        int shadow_fidelity = 3;
        bool shadow_blur = true; ///< ren_ShadowBlur (M211m)
        int mip_skip_levels = 0;
        f32 camera_scale_lod = 1.0f;
        int antialiasing = 0;
    };
    VideoOptions& video_options() { return video_options_; }
    const VideoOptions& video_options() const { return video_options_; }
    /// The highest graphics fidelity the device draws (Moho's
    /// graphics_FidelitySupported: 2, High, wherever ps_2_a runs).
    static constexpr int kFidelitySupported = 2;
    /// graphics_Fidelity as Moho draws by it: clamped to 0 (Low) through
    /// the supported (M213d).
    int fidelity() const {
        return std::clamp(video_options_.graphics_fidelity, 0, kFidelitySupported);
    }
    /// shadow_Fidelity as Moho draws by it, clamped to 0 (none) through 3
    /// (M211m): 0 casts nothing, 1 shades the terrain alone, 2 and 3 the
    /// meshes too (one tap; High's five at 3 with ren_ShadowBlur).
    static constexpr int kShadowFidelitySupported = 3;
    int shadow_fidelity() const {
        return std::clamp(video_options_.shadow_fidelity, 0, kShadowFidelitySupported);
    }
    /// SC_ToggleCursorClip (M217i): the cursor held inside a window (not full
    /// screen), or let go.
    void set_cursor_clip(bool on);
    bool cursor_clipped() const { return cursor_clipped_; }
    /// Whether the system's cursor shows over the window (it should not)
    bool system_cursor_shown() const;
    bool bloom_enabled() const { return bloom_enabled_; }
    /// What the scene clears to: Moho's black, with no glow (M210b). The sky
    /// dome draws over it; a test's own scenery may set another backdrop.
    static constexpr std::array<f32, 4> kClearColor = {0.0f, 0.0f, 0.0f, 0.0f};
    void set_clear_color(const std::array<f32, 4>& rgba) { clear_color_ = rgba; }
    u32 stored_decal_count() const { return static_cast<u32>(stored_decals_.size()); }
    /// The runtime decals and splats (M212c), as the last frame drew them.
    const RuntimeDecalRenderer& runtime_decals() const { return runtime_decals_; }
    /// The unit meshes the last frame drew, and the cubes drawn for those
    /// with none.
    u32 mesh_instance_count() const;
    u32 cube_instance_count() const { return unit_renderer_.cube_instance_count(); }
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

    // ── The window (M217h) ──
    /// Full screen on the primary display at a mode (FA's primary_adapter
    /// "w,h,fps"), or a decorated window at a size, placed there (else where
    /// it is), maximized or not ("windowed").
    void set_fullscreen(u32 width, u32 height, u32 rate);
    void set_windowed(u32 width, u32 height, std::optional<std::array<i32, 2>> position,
                      bool maximized);
    bool fullscreen() const;
    /// The primary display's modes (width, height, refresh), as it lists them.
    std::vector<std::array<u32, 3>> display_modes() const;
    /// The window's place and size while windowed (Moho's Windows.Main.*);
    /// nothing while full screen or without a window.
    struct WindowGeometry {
        i32 x = 0, y = 0;
        u32 width = 0, height = 0;
        bool maximized = false;
    };
    std::optional<WindowGeometry> windowed_geometry() const;
    /// vsync: FIFO; off, MAILBOX (IMMEDIATE where there's none). The
    /// swapchain is rebuilt before the next frame.
    void set_vsync(bool on);
    bool vsync() const { return vsync_; }
    VkPresentModeKHR present_mode() const { return present_mode_; }
    /// The framebuffer's size changed (the window resized): the swapchain is
    /// rebuilt before the next frame.
    void on_framebuffer_resized() { swapchain_stale_ = true; }
    /// Whether the next frame rebuilds the swapchain (tests read it).
    bool swapchain_stale() const { return swapchain_stale_; }
    /// Whether the swapchain's size changed since the last call (the UI's
    /// root frame follows it).
    bool take_resized() {
        const bool r = resized_;
        resized_ = false;
        return r;
    }

    /// Check if a mouse button is currently pressed.
    bool is_mouse_pressed(int glfw_button) const;

    /// Receives a captured frame (RGBA8, rows top to bottom).
    using CaptureCallback = std::function<void(ImageRGBA8)>;

    /// Capture the next frame exactly as presented: after bloom, HUD and UI,
    /// from either render() or render_ui_only(). The callback runs on this
    /// thread once the GPU has finished that frame. Returns false if the
    /// swapchain cannot be read back on this driver (no TRANSFER_SRC usage).
    bool request_capture(CaptureCallback on_captured);

    /// The scene as drawn, before the bloom and the UI: its colour and, in
    /// alpha, what glows (M211e), as floats.
    struct SceneImage {
        u32 width = 0, height = 0;
        std::vector<f32> rgba; ///< width*height*4, row-major from the top
    };
    using SceneCallback = std::function<void(SceneImage)>;
    /// Capture the next render() frame's scene (tests of what glows; M212d).
    /// The callback runs on this thread once the GPU has finished it.
    void request_scene_capture(SceneCallback on_captured) {
        pending_scene_capture_ = std::move(on_captured);
    }

    /// Vulkan validation errors reported so far (0 when validation is off).
    /// Screenshot / golden runs fail if this is non-zero.
    static u32 validation_error_count() { return validation_errors_.load(); }

    /// Advance the renderer's animation clock (UI animations, water, effects)
    /// by a fixed step per frame instead of wall-clock time, so frame N looks
    /// the same on every run. 0 restores wall-clock timing.
    void set_fixed_frame_dt(f32 dt) { fixed_frame_dt_ = dt; }

    static constexpr u32 SHADOW_MAP_SIZE = 4096;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

    /// What the last frame cost (M223b's render benchmark reads it after
    /// each render()): its draws and state changes, how long the CPU waited
    /// on the GPU's fence first, and the GPU's own figures for the latest
    /// frame whose results are in (a frame in flight behind).
    const CommandCounts& last_command_counts() const { return last_command_counts_; }
    f64 last_gpu_wait_ms() const { return last_gpu_wait_ms_; }
    const GpuFrameQueries::Frame& last_gpu_frame() const { return gpu_queries_.latest(); }
    /// How many world frames render() has recorded (the GPU figures name one).
    u64 frame_sequence() const { return frame_sequence_; }
    /// VRAM over the device-local heaps (VMA): what the renderer allocated,
    /// the memory blocks holding it (VMA's estimate of use without
    /// VK_EXT_memory_budget), and the budget.
    struct VramUsage {
        u64 allocated_bytes = 0;
        u64 used_bytes = 0;
        u64 budget_bytes = 0;
    };
    VramUsage vram_usage() const;
    /// The profiler's on-screen overlay, drawn while the profiler runs
    /// unless hidden (the render benchmark times zones without it).
    void set_profile_overlay_hidden(bool hidden) { profile_overlay_.set_hidden(hidden); }
    /// The GPU's name, as the driver gives it.
    const std::string& device_name() const { return device_name_; }

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
    /// A requested scene capture (request_scene_capture): recorded after the
    /// scene pass, before the bloom reads it.
    bool record_scene_capture(VkCommandBuffer cmd);
    void deliver_scene_capture();
    SceneCallback pending_scene_capture_;
    AllocatedBuffer scene_capture_buf_{};
    VkDeviceSize scene_capture_buf_size_ = 0;
    f32 fixed_frame_dt_ = 0.0f;
    bool capture_supported_ = false;
    bool vsync_ = true;
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool swapchain_stale_ = false; ///< rebuild before the next frame
    bool resized_ = false;         ///< the swapchain's size changed
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
    // The low fidelity terrain (M212h): terrain_layout_'s sets and push block
    VkPipeline terrain_low_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout terrain_low_layout_ = VK_NULL_HANDLE;
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
    /// The shields' pipelines (M211k), one per ShieldState: blended (Cybran,
    /// Aeon), blended unculled (UEF, Cybran's impact), added colour
    /// (Seraphim), added colour and glow (the impact), the fill's depth, and
    /// the personal shields' shells, blended with depth written (M211l).
    static constexpr u32 kShieldStates = 6;
    std::array<VkPipeline, kShieldStates> shield_pipelines_{};
    std::array<VkPipelineLayout, kShieldStates> shield_layouts_{};
    /// This frame's mesh draw calls by technique (mesh_draws).
    std::array<u32, 32> mesh_draws_{};
    VkPipeline decal_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout decal_layout_ = VK_NULL_HANDLE;
    // The glowing and glow-mask decals' (M212d): decal_layout_'s sets and push
    // block, each with a layout of its own that matches it.
    VkPipeline decal_glow_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout decal_glow_layout_ = VK_NULL_HANDLE;
    VkPipeline decal_glow_mask_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout decal_glow_mask_layout_ = VK_NULL_HANDLE;
    // The water's albedo decals (M212g), on its surface after it
    VkPipeline decal_water_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout decal_water_layout_ = VK_NULL_HANDLE;
    // The normal pass (M212e): the terrain's normals and the normal decals,
    // into the normal target the scene then reads.
    VkPipeline terrain_normal_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout terrain_normal_layout_ = VK_NULL_HANDLE;
    VkPipeline decal_normal_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout decal_normal_layout_ = VK_NULL_HANDLE;
    AllocatedImage terrain_normal_image_{};
    VkFramebuffer terrain_normal_framebuffer_ = VK_NULL_HANDLE;
    /// Point the terrain set's binding 26 at the normal target (a new scene,
    /// or a new target after a resize).
    void bind_normal_target();

    // Texture infrastructure
    VkDescriptorSetLayout texture_ds_layout_ = VK_NULL_HANDLE;
    VkSampler texture_sampler_ = VK_NULL_HANDLE;

    // Bone SSBO infrastructure (set=1 for mesh pipeline, per-frame)
    VkDescriptorSetLayout bone_ds_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool bone_ds_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet bone_ds_[FRAMES_IN_FLIGHT] = {};
    /// The unit renderer's bone SSBO each set was last written with
    /// (bone_ssbo_generation): the SSBO is a new buffer when it grows.
    u32 bone_ds_generation_[FRAMES_IN_FLIGHT] = {};
    /// Point slot `fi`'s bone set at the unit renderer's SSBO for that slot.
    void write_bone_descriptor(u32 fi);

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
    void create_terrain_strata_ubo(const std::vector<map::StratumInfo>& strata, u32 normal_tile_w,
                                   u32 normal_tile_h);
    void destroy_terrain_strata_ubo();

    // Sub-renderers
    TerrainMesh terrain_mesh_;
    UnitRenderer unit_renderer_;
    WaterRenderer water_renderer_;
    SkyRenderer sky_renderer_; // the map's sky dome (M210b)
    FogRenderer fog_renderer_;
    UIRenderer ui_renderer_;
    MovieTextures movie_textures_; ///< Movie controls' frames (M216a)
    OverlayRenderer overlay_renderer_;
    MinimapRenderer minimap_renderer_;
    std::vector<UIQuad> painted_minimap_; // FA minimap window's quads this frame (dump)
    StrategicIconRenderer strategic_icon_renderer_;
    VideoOptions video_options_;
    bool cursor_clipped_ = false;
    HudRenderer hud_renderer_;
    SelectionInfoRenderer selection_info_renderer_;
    ProfileOverlay profile_overlay_;
    ui::UIDispatch ui_dispatch_;
    f64 last_frame_time_ = 0.0;
    f32 total_time_ = 0.0f;
    f32 frame_dt_ = 0.0f;
    /// The waves' system clock: the frames' steps summed, so a test's fixed
    /// step runs it as it runs the particles (M213c).
    f64 wave_clock_ = 0.0;
    MeshCache mesh_cache_;
    TextureCache texture_cache_;
    FontCache font_cache_;
    Camera camera_;
    i32 player_army_ = 0;
    ReconView recon_;
    UserPlayableRect playable_rect_;
    bool fog_enabled_ = true;
    bool decals_enabled_ = true;
    bool economy_overlay_ = false;

    // The map's decals, projected and lit (M212b): each draws the terrain's
    // own triangles under it, a range of decal_indices_ over the terrain's
    // vertices, projected by its texture matrix.
    struct StoredDecal {
        std::string albedo_path;
        std::string spec_path; ///< empty: none (no specular)
        DecalTechnique technique = DecalTechnique::Albedo;
        f32 rotation_y = 0; ///< its turn, which turns a normal decal's normals (M212e)
        f32 u[4] = {};      ///< the texture matrix's u column (DecalsVS)
        f32 v[4] = {};      ///< its v (world z) column
        f32 mid_x = 0, mid_z = 0;
        f32 radius = 0; ///< its bounds' half diagonal, for the view's cull
        f32 cut_off_lod = 1000.0f;
        f32 near_cut_off_lod = 0.0f;
        u32 first_index = 0, index_count = 0;
    };
    std::vector<StoredDecal> stored_decals_;
    /// A decal this frame draws: the map's or a script's, its technique,
    /// textures, matrix, alpha (its LOD fade times its own), turn and
    /// triangles, from the map's index buffer or the scripts'.
    struct FrameDecal {
        DecalTechnique technique = DecalTechnique::Albedo;
        const std::string* albedo = nullptr;
        const std::string* spec = nullptr;
        const f32* u = nullptr;
        const f32* v = nullptr;
        f32 alpha = 1, rotation_y = 0;
        u32 first_index = 0, index_count = 0;
        bool runtime = false;
    };
    std::vector<FrameDecal> frame_decals_;
    /// This frame's decals, the map's then the scripts', seen and not faded
    /// out (M212e: the normal pass and the colour passes both draw them), at
    /// `ticks` (the sim's, and the frame's fraction of the next), which picks
    /// an animated texture's frame (M212g).
    void collect_frame_decals(const Frustum& frustum, f32 ticks);
    /// A decal texture's frames (Moho's CAnimTexture, M212g), by its name: it
    /// alone, or its numbered sequence. Each frame's load is begun when the
    /// list is made.
    std::map<std::string, std::vector<std::string>> decal_frames_;
    const std::vector<std::string>& decal_frames(const std::string& path);
    /// The texture a decal shows at `ticks`: `path`'s frame then, or `path`
    /// itself (empty, or one frame).
    const std::string* decal_frame(const std::string& path, f32 ticks);
    /// Draw this frame's decals of `technique` with `pipeline`, over the
    /// terrain's vertices, in the render pass open (the normal pass, or the
    /// scene's).
    void record_decals(VkCommandBuffer cmd, u32 fi, DecalTechnique technique, VkPipeline pipeline,
                       const std::array<f32, 16>& view_proj);
    AllocatedBuffer decal_indices_{};
    /// Retail's mask, which every decal's alpha takes (TerrainCommon).
    VkDescriptorSet decal_mask_ds_ = VK_NULL_HANDLE;
    /// Free the decals' index buffer (build_scene makes it for a map with
    /// decals). The device must be idle.
    void destroy_decal_buffers();

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
    /// The last frame's view of the world, whose entities the camera's
    /// targets follow (M217g): the app's history outlives it; a test drawing
    /// from its own snapshots must keep them while it polls.
    sim::FrameView camera_view_;
    /// Its game time, (tick + interpolant) x 0.1: the camera's game clock.
    f64 camera_game_time_ = 0.0;
    /// Bind the map's environment cubes and FA's lookups for meshes (M211a/b).
    void bind_mesh_environment(const map::ScmapEnvironment& environment);
    /// Clamped, for FA's lookup textures.
    VkSampler lookup_sampler_ = VK_NULL_HANDLE;
    /// The scene's lighting: its map's, else SCMP_009's.
    map::ScmapLighting lighting_{};
    bool terrain_xp_ = false;
    bool terrain_glow_ = false; ///< TTerrainGlow (M212f)
    TerrainTime terrain_time_;  ///< the terrain shader's Time (M212f)
    /// Write the lighting into every frame's UBO.
    void upload_lighting();

    // Particle system
    ParticleSystem particle_system_;
    WaveSystem wave_system_;
    std::vector<WaveParticle> waves_emitted_; ///< this frame's, for the particles
    ParticleRenderer particle_renderer_;
    RuntimeDecalRenderer runtime_decals_; // scripts' decals and splats (M212c)
    EmitterBlueprintCache emitter_bp_cache_;
    /// The map build_scene drew (its water, for particles; M214c).
    const map::Terrain* terrain_ = nullptr;
    BeamRenderer beam_renderer_;
    BeamBlueprintCache beam_bp_cache_;
    TrailRenderer trail_renderer_;
    TrailBlueprintCache trail_bp_cache_;

    // Bloom post-processing
    bool bloom_enabled_ = true;
    std::array<f32, 4> clear_color_ = kClearColor;
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
    /// One between them, which goes on from the first and ends as it does:
    /// on a map with water, before the refracting particles' copy (M214d).
    VkRenderPass scene_middle_pass_ = VK_NULL_HANDLE;
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
    /// End the scene pass open, copy the frame (copy_refraction), and go on
    /// drawing in `next`, one of the scene's passes that load (M214d).
    void copy_and_continue(VkCommandBuffer cmd, VkRenderPass next);
    /// Which meshes a draw_meshes call draws (M213b): Moho's buckets
    /// before and after the water, or the units in its reflection.
    /// Moho's mesh stages: before the water (All on a map without one),
    /// after it, into its reflection, and after the effects above it, the
    /// shields' (M211k).
    enum class MeshPass { All, BeforeWater, AfterWater, Reflection, AfterEffects };
    /// The mesh instances' draws, with the scene's pipelines, seen by `vp`
    /// (for the reflection, already mirrored).
    void draw_meshes(VkCommandBuffer cmd, u32 fi, const std::array<f32, 16>& vp, MeshPass stage);
    void create_bloom_pipelines();
    void destroy_bloom_resources();

    // Frame-in-flight tracking
    u32 frame_index_ = 0;

    // A frame's cost, for the render benchmark (M223b)
    GpuFrameQueries gpu_queries_;
    std::string device_name_;
    CommandCounts last_command_counts_;
    f64 last_gpu_wait_ms_ = 0;
    u64 frame_sequence_ = 0;

    // Cleanup
    DeletionQueue deletion_queue_;
    bool initialized_ = false;
    bool caches_initialized_ = false;
};

} // namespace osc::renderer
