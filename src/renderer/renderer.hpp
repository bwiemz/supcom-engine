#pragma once

#include "renderer/camera.hpp"
#include "renderer/mesh_cache.hpp"
#include "renderer/terrain_mesh.hpp"
#include "renderer/texture_cache.hpp"
#include "renderer/unit_renderer.hpp"
#include "renderer/water_renderer.hpp"
#include "renderer/vk_types.hpp"
#include "core/types.hpp"

#include <string>

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

    /// Render one frame (updates unit instances, draws everything).
    void render(const sim::SimState& sim, lua_State* L);

    /// Returns true if the window close was requested.
    bool should_close() const;

    /// Poll window events and update camera.
    void poll_events(f64 dt);

    /// Clean up all Vulkan resources.
    void shutdown();

    /// Mouse scroll callback (called from GLFW callback).
    void on_scroll(f64 y_offset);

    Camera& camera() { return camera_; }

private:
    bool create_swapchain(u32 width, u32 height);
    void create_depth_image();
    void create_render_pass();
    void create_framebuffers();
    void create_pipelines();
    void recreate_swapchain();

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
    VkFormat swapchain_format_ = VK_FORMAT_B8G8R8A8_SRGB;
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;

    // Depth
    AllocatedImage depth_image_{};
    VkFormat depth_format_ = VK_FORMAT_D32_SFLOAT;

    // Render pass & framebuffers
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;

    // Command pool & buffer
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_buf_ = VK_NULL_HANDLE;

    // Sync
    VkFence render_fence_ = VK_NULL_HANDLE;
    VkSemaphore present_semaphore_ = VK_NULL_HANDLE;
    VkSemaphore render_semaphore_ = VK_NULL_HANDLE;

    // Pipelines
    VkPipeline terrain_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout terrain_layout_ = VK_NULL_HANDLE;
    VkPipeline unit_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout unit_layout_ = VK_NULL_HANDLE;
    VkPipeline water_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout water_layout_ = VK_NULL_HANDLE;
    VkPipeline mesh_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout mesh_layout_ = VK_NULL_HANDLE;

    // Texture infrastructure
    VkDescriptorSetLayout texture_ds_layout_ = VK_NULL_HANDLE;
    VkSampler texture_sampler_ = VK_NULL_HANDLE;

    // Bone SSBO infrastructure (set=1 for mesh pipeline)
    VkDescriptorSetLayout bone_ds_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool bone_ds_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet bone_ds_ = VK_NULL_HANDLE;

    // Sub-renderers
    TerrainMesh terrain_mesh_;
    UnitRenderer unit_renderer_;
    WaterRenderer water_renderer_;
    MeshCache mesh_cache_;
    TextureCache texture_cache_;
    Camera camera_;

    // Cleanup
    DeletionQueue deletion_queue_;
    bool initialized_ = false;
};

} // namespace osc::renderer
