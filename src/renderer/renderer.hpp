#pragma once

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
#include "renderer/unit_renderer.hpp"
#include "renderer/water_renderer.hpp"
#include "renderer/fog_renderer.hpp"
#include "renderer/particle_system.hpp"
#include "renderer/particle_renderer.hpp"
#include "renderer/emitter_blueprint.hpp"
#include "renderer/normal_overlay.hpp"
#include "renderer/vk_types.hpp"
#include "core/types.hpp"

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
class SimState;
}

namespace osc::renderer {

class Renderer {
public:
    /// Initialize Vulkan, GLFW window, and all pipelines.
    /// Returns false if Vulkan is unavailable (fall back to headless).
    bool init(u32 width, u32 height, const std::string& title);

    /// One-time scene upload (terrain mesh, static buffers, mesh preload).
    void build_scene(const sim::SimState& sim, vfs::VirtualFileSystem* vfs,
                     lua_State* L);

    /// Tear down scene-specific GPU resources for map reload.
    void clear_scene();

    /// Render one frame (updates unit instances, draws everything).
    void render(sim::SimState& sim, lua_State* L,
                ui::UIControlRegistry* ui_registry = nullptr,
                const std::unordered_set<u32>* selected_ids = nullptr);

    /// Render only the UI layer (no 3D scene, no bloom).
    /// Used during loading screen when SimState doesn't exist.
    void render_ui_only(lua_State* L, ui::UIControlRegistry* ui_registry);

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

    /// Update the window title bar text.
    void set_window_title(const char* title);

    Camera& camera() { return camera_; }
    const Camera& camera() const { return camera_; }
    void set_player_army(i32 army) { player_army_ = army; }
    i32 player_army() const { return player_army_; }
    void set_fog_enabled(bool enabled) { fog_enabled_ = enabled; }
    bool fog_enabled() const { return fog_enabled_; }
    void set_decals_enabled(bool enabled) { decals_enabled_ = enabled; }
    bool decals_enabled() const { return decals_enabled_; }
    void set_bloom_enabled(bool b) { bloom_enabled_ = b; }
    bool bloom_enabled() const { return bloom_enabled_; }
    u32 stored_decal_count() const { return static_cast<u32>(stored_decals_.size()); }
    const MinimapRenderer& minimap() const { return minimap_renderer_; }
    u32 width() const { return window_width_; }
    u32 height() const { return window_height_; }

    /// Get current mouse position in screen pixels.
    void mouse_position(f64& x, f64& y) const;

    /// Check if a mouse button is currently pressed.
    bool is_mouse_pressed(int glfw_button) const;

    static constexpr u32 SHADOW_MAP_SIZE = 4096;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

private:
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
    VkSemaphore render_semaphore_[FRAMES_IN_FLIGHT] = {};

    // Pipelines
    VkPipeline terrain_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout terrain_layout_ = VK_NULL_HANDLE;
    VkPipeline unit_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout unit_layout_ = VK_NULL_HANDLE;
    VkPipeline water_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout water_layout_ = VK_NULL_HANDLE;
    VkPipeline mesh_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout mesh_layout_ = VK_NULL_HANDLE;
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
    f32 terrain_strata_scales_[9] = {};

    // Sub-renderers
    TerrainMesh terrain_mesh_;
    UnitRenderer unit_renderer_;
    WaterRenderer water_renderer_;
    FogRenderer fog_renderer_;
    UIRenderer ui_renderer_;
    OverlayRenderer overlay_renderer_;
    MinimapRenderer minimap_renderer_;
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
    bool fog_enabled_ = true;
    bool decals_enabled_ = true;
    bool b_key_was_pressed_ = false;

    // Decal rendering
    AllocatedBuffer decal_quad_verts_{};
    AllocatedBuffer decal_quad_indices_{};
    AllocatedBuffer decal_instance_buf_[FRAMES_IN_FLIGHT] = {};
    void* decal_instance_mapped_[FRAMES_IN_FLIGHT] = {};

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

    // Particle system
    ParticleSystem particle_system_;
    ParticleRenderer particle_renderer_;
    EmitterBlueprintCache emitter_bp_cache_;

    // Bloom post-processing
    bool bloom_enabled_ = true;
    f32 bloom_threshold_ = 0.8f;
    f32 bloom_intensity_ = 1.2f;
    f32 bloom_strength_ = 0.3f;

    // Offscreen scene image (rendered instead of swapchain, then composited)
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
