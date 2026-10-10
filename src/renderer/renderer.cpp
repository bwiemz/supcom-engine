#define VMA_IMPLEMENTATION
#include "renderer/renderer.hpp"
#include "renderer/vk_cmd.hpp"
#include "core/cursor.hpp"
#include "core/fullscreen.hpp"
#include "core/ui_registry_keys.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
#include "renderer/dds_decode.hpp"
#include "vfs/virtual_file_system.hpp"
#include "renderer/dds_parser.hpp"
#include "platform/native_fullscreen.hpp"
#include "platform/paths.hpp"
#include "core/profiler.hpp"
#include "renderer/pipeline_builder.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/terrain_mesh.hpp"
#include "sim/game_colors.hpp"
#include "sim/scm_parser.hpp"
#include "sim/world_snapshot.hpp"
#include "map/terrain.hpp"
#include "renderer/terrain_normal_maps.hpp"
#include "renderer/frustum.hpp"
#include "renderer/decal_math.hpp"
#include "map/pathfinding_grid.hpp"
#include "map/intel_grid.hpp"
#include "ui/world_view.hpp"

#include <VkBootstrap.h>
#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>
#include <ostream>
#include <unordered_map>

/// Log a Vulkan/VMA error with file and line context.
#define VK_CHECK(call) do { \
    VkResult vk_check_res_ = (call); \
    if (vk_check_res_ != VK_SUCCESS) \
        spdlog::error("Vulkan error {} at {}:{}: {}", \
                      static_cast<int>(vk_check_res_), __FILE__, __LINE__, #call); \
} while(0)

namespace osc::renderer {

namespace {
/// Alpha carries the frame's glow (M211e): what doesn't glow writes colour only.
constexpr VkColorComponentFlags kColorOnly =
    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
constexpr VkColorComponentFlags kColorAndGlow = kColorOnly | VK_COLOR_COMPONENT_A_BIT;
/// FA's bloom tuning (faf-re CRenFrame, CBloomRenderer): ren_BloomGlowCopyScale,
/// ren_BloomBlurKernelScale and ren_BloomBlurCount, as the game ships them.
constexpr f32 kBloomGlowCopyScale = 2.0f;
constexpr f32 kBloomBlurKernelScale = 1.5f;
constexpr int kBloomBlurCount = 2;
/// `view_proj` (column-major) seen through the water's plane y = e, as
/// Moho mirrors its view for the reflection (M213b): the world's (x, 2e - y,
/// z). Its columns: y's negated, and 2e of y's added to the translation.
std::array<f32, 16> mirrored_view_proj(const std::array<f32, 16>& vp, f32 e) {
    std::array<f32, 16> m = vp;
    for (size_t r = 0; r < 4; ++r) {
        m[4 + r] = -vp[4 + r];
        m[12 + r] = vp[12 + r] + 2.0f * e * vp[4 + r];
    }
    return m;
}

} // namespace

// GLFW scroll callback — forward to Renderer via user pointer
static void glfw_scroll_callback(GLFWwindow* window, double /*xoffset*/,
                                 double yoffset) {
    auto* renderer =
        static_cast<Renderer*>(glfwGetWindowUserPointer(window));
    if (renderer) renderer->on_scroll(yoffset);
}

void Renderer::on_scroll(f64 y_offset) {
    // A wheel notch over the world zooms about the cursor (CUIWorldView's
    // HandleEvent): the pivot, then CameraZoom
    if (!window_ || !camera_.accepts_mouse()) return;
    f64 mx = 0;
    f64 my = 0;
    mouse_position(mx, my);
    camera_.set_pivot(static_cast<f32>(mx), static_cast<f32>(my));
    camera_.zoom(static_cast<f32>(y_offset));
}

bool Renderer::init(u32 width, u32 height, const std::string& title,
                    bool offscreen) {
    offscreen_ = offscreen;
    // GLFW
    if (!glfwInit()) {
        spdlog::error("Failed to initialize GLFW");
        return false;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, offscreen ? GLFW_FALSE : GLFW_TRUE);
    // Offscreen keeps a hidden window (input and timing code expect one) but
    // never shows it: GLFW's show waits for the compositor to map the window.
    glfwWindowHint(GLFW_VISIBLE, offscreen ? GLFW_FALSE : GLFW_TRUE);
    // An offscreen capture is its size in pixels, unscaled on a Retina display
    glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, offscreen ? GLFW_FALSE : GLFW_TRUE);
    window_ = glfwCreateWindow(static_cast<int>(width),
                               static_cast<int>(height),
                               title.c_str(), nullptr, nullptr);
    if (!window_) {
        spdlog::error("Failed to create GLFW window");
        glfwTerminate();
        return false;
    }
    // The system's cursor stays hidden until FA's cursor is made it
    // (update_system_cursor); offscreen, the engine draws FA's
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
    window_width_ = width;
    window_height_ = height;
    camera_.set_viewport(static_cast<f32>(width), static_cast<f32>(height));
    // The camera's targets are the world's entities as they are drawn
    camera_.set_entity_lookup([this](u32 id, CameraEntityPose& out) {
        const sim::EntityRecord* e = camera_view_.find(id);
        if (!e) return false;
        const sim::Vector3 p = camera_view_.position(*e);
        const sim::Quaternion q = camera_view_.orientation(*e);
        out.pos = {p.x, p.y, p.z};
        out.orient = {q.x, q.y, q.z, q.w};
        return true;
    });

    glfwSetWindowUserPointer(window_, this);
    glfwSetScrollCallback(window_, glfw_scroll_callback);
    // A resize rebuilds the swapchain before the next frame (M217h): Wayland
    // and some drivers never report the old one out of date
    glfwSetFramebufferSizeCallback(window_, [](GLFWwindow* w, int, int) {
        if (auto* r = static_cast<Renderer*>(glfwGetWindowUserPointer(w)))
            r->on_framebuffer_resized();
    });
    ui_dispatch_.install_callbacks(window_);

    // Vulkan instance (vk-bootstrap). Validation: on in debug builds, off in
    // release; OSC_VK_VALIDATION=0/1 overrides either way.
    bool validation =
#ifdef NDEBUG
        false;
#else
        true;
#endif
    if (const char* v = std::getenv("OSC_VK_VALIDATION")) validation = v[0] == '1';
#ifdef OSC_VULKAN_LAYER_PATH
    if (validation) platform::set_env_default("VK_ADD_LAYER_PATH", OSC_VULKAN_LAYER_PATH);
#endif
    if (validation) {
        auto sys = vkb::SystemInfo::get_system_info();
        if (!sys || !sys.value().validation_layers_available) {
            spdlog::warn("Vulkan validation requested but VK_LAYER_KHRONOS_validation "
                         "is not installed; running without it");
            validation = false;
        }
    }
    vkb::InstanceBuilder inst_builder;
    inst_builder.set_app_name("OpenSupCom")
        .request_validation_layers(validation)
        .require_api_version(1, 0, 0);
    if (offscreen && std::getenv("OSC_HEADLESS_SURFACE")) {
        inst_builder.set_headless(true)
            .enable_extension(VK_KHR_SURFACE_EXTENSION_NAME)
            .enable_extension(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
    }
    if (validation) inst_builder.set_debug_callback(&Renderer::vulkan_debug_callback);
    auto inst_ret = inst_builder.build();
    if (inst_ret) {
        spdlog::info("Vulkan validation: {}", validation ? "enabled" : "disabled");
    }

    if (!inst_ret) {
        spdlog::error("Failed to create Vulkan instance: {}",
                      inst_ret.error().message());
        glfwDestroyWindow(window_);
        glfwTerminate();
        return false;
    }

    auto vkb_inst = inst_ret.value();
    instance_ = vkb_inst.instance;
    debug_messenger_ = vkb_inst.debug_messenger;

    // Surface
    if (offscreen && std::getenv("OSC_HEADLESS_SURFACE")) {
        auto create_headless = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateHeadlessSurfaceEXT"));
        VkHeadlessSurfaceCreateInfoEXT hs_ci{};
        hs_ci.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
        if (!create_headless ||
            create_headless(instance_, &hs_ci, nullptr, &surface_) != VK_SUCCESS) {
            spdlog::error("Offscreen rendering needs VK_EXT_headless_surface, "
                          "which this Vulkan driver does not provide");
            return false;
        }
        spdlog::info("Rendering offscreen (headless surface)");
    } else if (glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) !=
               VK_SUCCESS) {
        spdlog::error("Failed to create window surface");
        return false;
    }

    // Physical device — require BC texture compression + anisotropic filtering
    VkPhysicalDeviceFeatures required_features{};
    required_features.textureCompressionBC = VK_TRUE;
    required_features.samplerAnisotropy = VK_TRUE;

    vkb::PhysicalDeviceSelector selector(vkb_inst);
    auto phys_ret = selector
        .set_surface(surface_)
        .set_minimum_version(1, 0)
        .set_required_features(required_features)
        .select();

    if (!phys_ret) {
        spdlog::error("No suitable Vulkan GPU found: {}",
                      phys_ret.error().message());
        return false;
    }

    auto vkb_phys = phys_ret.value();
    physical_device_ = vkb_phys.physical_device;
    spdlog::info("Vulkan GPU: {}", vkb_phys.name);
    device_name_ = vkb_phys.name;

    // Pipeline statistics, where the device has them: the render benchmark's
    // primitive and shader counts (M223b).
    VkPhysicalDeviceFeatures optional_features{};
    optional_features.pipelineStatisticsQuery = VK_TRUE;
    const bool pipeline_statistics = vkb_phys.enable_features_if_present(optional_features);

    // Logical device
    vkb::DeviceBuilder dev_builder(vkb_phys);
    auto dev_ret = dev_builder.build();
    if (!dev_ret) {
        spdlog::error("Failed to create Vulkan device: {}",
                      dev_ret.error().message());
        return false;
    }

    const auto& vkb_dev = dev_ret.value();
    device_ = vkb_dev.device;

    auto gq = vkb_dev.get_queue(vkb::QueueType::graphics);
    auto gqi = vkb_dev.get_queue_index(vkb::QueueType::graphics);
    if (!gq || !gqi) {
        spdlog::error("Failed to get graphics queue");
        return false;
    }
    graphics_queue_ = gq.value();
    graphics_queue_family_ = gqi.value();
    gpu_queries_.init(physical_device_, device_, graphics_queue_family_, pipeline_statistics,
                      FRAMES_IN_FLIGHT);

    // VMA allocator
    VmaAllocatorCreateInfo alloc_ci{};
    alloc_ci.physicalDevice = physical_device_;
    alloc_ci.device = device_;
    alloc_ci.instance = instance_;
    if (vmaCreateAllocator(&alloc_ci, &allocator_) != VK_SUCCESS) {
        spdlog::error("Failed to create VMA allocator");
        return false;
    }

    // Swapchain
    if (!create_swapchain(width, height)) return false;

    // Depth image, with a stencil for the range overlays' volumes (FA's
    // D24S8): the first of these the device can attach
    for (const VkFormat f : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(physical_device_, f, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            depth_format_ = f;
            break;
        }
    }
    if (!has_stencil()) spdlog::warn("No depth-stencil format: range overlays won't draw");
    create_depth_image();

    // Render pass & framebuffers
    create_render_pass();
    create_framebuffers();

    // Command pool + buffer
    VkCommandPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.queueFamilyIndex = graphics_queue_family_;
    pool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_CHECK(vkCreateCommandPool(device_, &pool_ci, nullptr, &cmd_pool_));

    VkCommandBufferAllocateInfo cmd_ai{};
    cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool = cmd_pool_;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = FRAMES_IN_FLIGHT;
    vkAllocateCommandBuffers(device_, &cmd_ai, cmd_buf_);

    // Sync objects (one set per frame in flight)
    VkFenceCreateInfo fence_ci{};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkSemaphoreCreateInfo sem_ci{};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        VK_CHECK(vkCreateFence(device_, &fence_ci, nullptr, &render_fence_[i]));
        VK_CHECK(vkCreateSemaphore(device_, &sem_ci, nullptr, &present_semaphore_[i]));
    }

    // Texture descriptor set layout (set=0, binding=0: combined image sampler)
    {
        VkDescriptorSetLayoutBinding sampler_binding{};
        sampler_binding.binding = 0;
        sampler_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampler_binding.descriptorCount = 1;
        sampler_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo ds_ci{};
        ds_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_ci.bindingCount = 1;
        ds_ci.pBindings = &sampler_binding;
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &ds_ci, nullptr, &texture_ds_layout_));
    }

    // Bone SSBO descriptor set layout (set=1, binding=0: storage buffer)
    {
        VkDescriptorSetLayoutBinding ssbo_binding{};
        ssbo_binding.binding = 0;
        ssbo_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ssbo_binding.descriptorCount = 1;
        ssbo_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo ds_ci{};
        ds_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_ci.bindingCount = 1;
        ds_ci.pBindings = &ssbo_binding;
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &ds_ci, nullptr, &bone_ds_layout_));
    }

    // Terrain texture descriptor set layout (set=0): bindings 0-1: blend maps,
    // 2-10: stratum albedo, 11-19: stratum normal, 20: fog of war, 21: the
    // map's normal maps (M212e), 22: the upper stratum's albedo (all combined
    // image samplers), 23: the strata's sizes (a uniform buffer, M212a).
    {
        // 24 and 25: the water ramp and the water map (M213a); 26: the normal
        // target (M212e).
        std::array<VkDescriptorSetLayoutBinding, 27> terrain_bindings{};
        for (u32 i = 0; i < 27; i++) {
            terrain_bindings[i].binding = i;
            terrain_bindings[i].descriptorType = i == kTerrainStrataBinding
                                                     ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                     : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            terrain_bindings[i].descriptorCount = 1;
            terrain_bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }

        VkDescriptorSetLayoutCreateInfo ds_ci{};
        ds_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_ci.bindingCount = static_cast<u32>(terrain_bindings.size());
        ds_ci.pBindings = terrain_bindings.data();
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &ds_ci, nullptr,
                                    &terrain_tex_ds_layout_));
    }

    // Texture sampler (trilinear, anisotropic, repeat wrap)
    {
        VkSamplerCreateInfo sampler_ci{};
        sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_ci.magFilter = VK_FILTER_LINEAR;
        sampler_ci.minFilter = VK_FILTER_LINEAR;
        sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_ci.anisotropyEnable = VK_TRUE;
        sampler_ci.maxAnisotropy = 8.0f;
        sampler_ci.maxLod = 16.0f;
        VK_CHECK(vkCreateSampler(device_, &sampler_ci, nullptr, &texture_sampler_));
        // FA's lookup textures clamp (mesh.fx anisotropicSampler, insectSampler)
        sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.anisotropyEnable = VK_FALSE;
        VK_CHECK(vkCreateSampler(device_, &sampler_ci, nullptr, &lookup_sampler_));
    }

    // Shadow resources (must be created before pipelines — shadow_ds_layout_ is referenced)
    create_shadow_resources();

    // The frame's targets (must be created before pipelines — frame_.scene_pass() is
    // needed for scene pipeline builds now that all scene draws target offscreen HDR)
    create_frame_targets();

    // Pipelines (scene pipelines built against frame_.scene_pass())
    create_pipelines();

    // Shadow depth-only pipelines (need shadow_map_'s pass + bone_ds_layout_)
    shadow_casters_.create(device_, shadow_map_.map_pass(), bone_ds_layout_, texture_ds_layout_);

    // Bloom post-processing pipelines (fullscreen triangle passes)
    bloom_.create_pipelines(render_pass_, texture_ds_layout_);

    // UI renderer instance buffer
    ui_renderer_.init(device_, allocator_);
    movie_textures_.init(device_, allocator_, &texture_cache_);
    ui_renderer_.set_movie_textures(&movie_textures_);

    // Overlay renderer (health bars, selection, command lines)
    overlay_renderer_.init(device_, allocator_);

    // FA's particles, in the scene pass (M214c)
    particle_renderer_.init(device_, allocator_, frame_.scene_pass(), texture_ds_layout_);
    // Scripts' decals and splats, in the scene pass (M212c)
    runtime_decals_.init(device_, allocator_, frame_.scene_pass(), terrain_tex_ds_layout_,
                         shadow_ds_layout_, texture_ds_layout_);
    // FA's beams, in the scene pass too (M214a)
    beam_renderer_.init(device_, allocator_, frame_.scene_pass(), texture_ds_layout_);
    command_graph_renderer_.init(device_, allocator_, frame_.scene_pass(), texture_ds_layout_);
    selection_renderer_.init(device_, allocator_, frame_.scene_pass(), texture_ds_layout_);
    if (has_stencil()) range_renderer_.init(device_, allocator_, frame_.scene_pass());
    resource_icon_renderer_.init(device_, allocator_, frame_.scene_pass(), texture_ds_layout_);
    // FA's trails, likewise (M214b)
    trail_renderer_.init(device_, allocator_, frame_.scene_pass(), texture_ds_layout_);
    // FA's sky (M210b)
    sky_renderer_.init(device_, allocator_, frame_.scene_pass());
    // FA's water (M213a)
    water_renderer_.init(device_, allocator_, frame_.scene_pass());
    water_renderer_.set_frame_images(
        {.refraction = frame_.refraction().view, .reflection = frame_.reflection().view});
    // The refracting particles bend the same copy, made again for them.
    particle_renderer_.set_background(frame_.refraction().view);

    // Minimap renderer
    minimap_renderer_.init(device_, allocator_);

    // Strategic icon renderer
    strategic_icon_renderer_.init(device_, allocator_);

    // What the player's intel shows, for everything that draws units (M215a)
    unit_renderer_.set_recon(&recon_);
    unit_renderer_.set_playable_rect(&playable_rect_);
    strategic_icon_renderer_.set_recon(&recon_);
    overlay_renderer_.set_recon(&recon_);
    minimap_renderer_.set_icons(&strategic_icon_renderer_);
    particle_system_.set_recon(&recon_);
    overlay_renderer_.set_beams(&beam_renderer_);
    overlay_renderer_.set_trails(&trail_renderer_);
    overlay_renderer_.set_particles(&particle_system_);

    // HUD renderer (economy bars)
    hud_renderer_.init(device_, allocator_);

    // Selection info panel
    selection_info_renderer_.init(device_, allocator_);

    // Profile overlay
    profile_overlay_.init(device_, allocator_);

    initialized_ = true;
    spdlog::info("Renderer initialized ({}x{})", width, height);
    return true;
}

bool Renderer::create_swapchain(u32 width, u32 height) {
    // Frame capture (screenshots, golden images) copies out of the swapchain
    // image, which needs TRANSFER_SRC usage -- only request it if supported.
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);
    capture_supported_ =
        (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

    vkb::SwapchainBuilder builder(physical_device_, device_, surface_);
    builder.set_desired_format({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
        .set_desired_present_mode(vsync_ ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_MAILBOX_KHR)
        .add_fallback_present_mode(vsync_ ? VK_PRESENT_MODE_FIFO_KHR
                                          : VK_PRESENT_MODE_IMMEDIATE_KHR)
        .add_fallback_present_mode(VK_PRESENT_MODE_FIFO_KHR)
        .set_desired_extent(width, height)
        .set_old_swapchain(swapchain_);
    if (capture_supported_) {
        builder.add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    }
    auto sc_ret = builder.build();

    if (!sc_ret) {
        spdlog::error("Failed to create swapchain: {}",
                      sc_ret.error().message());
        return false;
    }

    auto vkb_sc = sc_ret.value();
    swapchain_ = vkb_sc.swapchain;
    swapchain_format_ = vkb_sc.image_format;
    swapchain_images_ = vkb_sc.get_images().value();
    swapchain_image_views_ = vkb_sc.get_image_views().value();

    if (window_width_ != vkb_sc.extent.width || window_height_ != vkb_sc.extent.height)
        resized_ = true;
    window_width_ = vkb_sc.extent.width;
    window_height_ = vkb_sc.extent.height;
    present_mode_ = vkb_sc.present_mode;

    // One render-finished semaphore per image (the device is idle here: first
    // creation, or recreate_swapchain() after vkDeviceWaitIdle).
    for (auto sem : render_finished_) vkDestroySemaphore(device_, sem, nullptr);
    render_finished_.assign(swapchain_images_.size(), VK_NULL_HANDLE);
    VkSemaphoreCreateInfo sem_ci{};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (auto& sem : render_finished_) {
        VK_CHECK(vkCreateSemaphore(device_, &sem_ci, nullptr, &sem));
    }

    return true;
}

void Renderer::create_depth_image() {
    VkImageCreateInfo img_ci{};
    img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.imageType = VK_IMAGE_TYPE_2D;
    img_ci.format = depth_format_;
    img_ci.extent = {window_width_, window_height_, 1};
    img_ci.mipLevels = 1;
    img_ci.arrayLayers = 1;
    img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    VK_CHECK(vmaCreateImage(allocator_, &img_ci, &alloc_ci,
                   &depth_image_.image, &depth_image_.allocation, nullptr));

    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = depth_image_.image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = depth_format_;
    view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (has_stencil()) view_ci.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    view_ci.subresourceRange.levelCount = 1;
    view_ci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(device_, &view_ci, nullptr, &depth_image_.view));
}

void Renderer::create_render_pass() {
    VkAttachmentDescription color_att{};
    color_att.format = swapchain_format_;
    color_att.samples = VK_SAMPLE_COUNT_1_BIT;
    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depth_att{};
    depth_att.format = depth_format_;
    depth_att.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_att.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_att.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depth_ref{};
    depth_ref.attachment = 1;
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    std::array<VkAttachmentDescription, 2> attachments = {color_att, depth_att};

    std::array<VkSubpassDependency, 2> deps{};
    // Incoming: external writes complete before we start
    // The depth image is shared by every frame in flight, so the previous
    // frame's depth writes (early AND late tests) must finish before this
    // frame clears and writes it.
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    // Outgoing: color writes visible to subsequent fragment reads (bloom compatibility)
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rp_ci{};
    rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_ci.attachmentCount = static_cast<u32>(attachments.size());
    rp_ci.pAttachments = attachments.data();
    rp_ci.subpassCount = 1;
    rp_ci.pSubpasses = &subpass;
    rp_ci.dependencyCount = static_cast<u32>(deps.size());
    rp_ci.pDependencies = deps.data();

    VK_CHECK(vkCreateRenderPass(device_, &rp_ci, nullptr, &render_pass_));
}

void Renderer::create_framebuffers() {
    framebuffers_.resize(swapchain_image_views_.size());
    for (size_t i = 0; i < swapchain_image_views_.size(); i++) {
        std::array<VkImageView, 2> views = {swapchain_image_views_[i],
                                             depth_image_.view};

        VkFramebufferCreateInfo fb_ci{};
        fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_ci.renderPass = render_pass_;
        fb_ci.attachmentCount = static_cast<u32>(views.size());
        fb_ci.pAttachments = views.data();
        fb_ci.width = window_width_;
        fb_ci.height = window_height_;
        fb_ci.layers = 1;

        VK_CHECK(vkCreateFramebuffer(device_, &fb_ci, nullptr, &framebuffers_[i]));
    }
}

void Renderer::create_shadow_resources() {
    // Moho's shadow map (M210c): its targets, passes and blur.
    shadow_map_.create(device_, allocator_, SHADOW_MAP_SIZE, texture_ds_layout_);

    // --- Light UBO (LightUboData, persistently mapped, per-frame for FIF safety) ---
    static_assert(sizeof(LightUboData) == 160, "LightUBO is std140: a mat4 and six vec4s");
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        VkBufferCreateInfo ubo_ci{};
        ubo_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ubo_ci.size = sizeof(LightUboData);
        ubo_ci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo info{};
        VK_CHECK(vmaCreateBuffer(allocator_, &ubo_ci, &alloc_ci,
                        &light_ubo_[i].buffer, &light_ubo_[i].allocation, &info));
        light_ubo_mapped_[i] = info.pMappedData;
    }
    upload_lighting();

    // --- Shadow descriptor set layout (binding 0: the shadow map, point-sampled,
    // binding 1: light UBO, bindings 2-4: the environment cubes meshes reflect,
    // "<default>", "<aeon>" and "<seraphim>", M211a/b; 5-6: FA's anisotropic and
    // insect lookups, M211b; 7: the shadow map, bilinear, white outside, the
    // meshes' PCF; 8: the terrain's mask, likewise, M210c) ---
    {
        std::array<VkDescriptorSetLayoutBinding, 9> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;

        for (u32 b = 2; b < 9; ++b) {
            bindings[b].binding = b;
            bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[b].descriptorCount = 1;
            bindings[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }

        VkDescriptorSetLayoutCreateInfo ds_ci{};
        ds_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_ci.bindingCount = static_cast<u32>(bindings.size());
        ds_ci.pBindings = bindings.data();
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &ds_ci, nullptr, &shadow_ds_layout_));
    }

    // --- Shadow descriptor pool + per-frame sets ---
    {
        std::array<VkDescriptorPoolSize, 2> pool_sizes{};
        pool_sizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 * FRAMES_IN_FLIGHT};
        pool_sizes[1] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FRAMES_IN_FLIGHT};

        VkDescriptorPoolCreateInfo pool_ci{};
        pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_ci.maxSets = FRAMES_IN_FLIGHT;
        pool_ci.poolSizeCount = static_cast<u32>(pool_sizes.size());
        pool_ci.pPoolSizes = pool_sizes.data();
        VK_CHECK(vkCreateDescriptorPool(device_, &pool_ci, nullptr, &shadow_ds_pool_));

        for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
            VkDescriptorSetAllocateInfo alloc_info{};
            alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc_info.descriptorPool = shadow_ds_pool_;
            alloc_info.descriptorSetCount = 1;
            alloc_info.pSetLayouts = &shadow_ds_layout_;
            VK_CHECK(vkAllocateDescriptorSets(device_, &alloc_info, &shadow_ds_[i]));

            VkDescriptorImageInfo img_info{};
            img_info.sampler = shadow_map_.point_sampler();
            img_info.imageView = shadow_map_.map_view();
            img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorImageInfo pcf_info = img_info;
            pcf_info.sampler = shadow_map_.linear_sampler();
            VkDescriptorImageInfo mask_info = pcf_info;
            mask_info.imageView = shadow_map_.mask_view();

            VkDescriptorBufferInfo buf_info{};
            buf_info.buffer = light_ubo_[i].buffer;
            buf_info.offset = 0;
            buf_info.range = sizeof(LightUboData);

            std::array<VkWriteDescriptorSet, 4> writes{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = shadow_ds_[i];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].pImageInfo = &img_info;

            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = shadow_ds_[i];
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[1].pBufferInfo = &buf_info;
            writes[2] = writes[0];
            writes[2].dstBinding = 7;
            writes[2].pImageInfo = &pcf_info;
            writes[3] = writes[0];
            writes[3].dstBinding = 8;
            writes[3].pImageInfo = &mask_info;

            vkc::update_descriptor_sets(device_, static_cast<u32>(writes.size()), writes.data(), 0,
                                        nullptr);
        }
    }

    spdlog::info("Shadow resources created (the map, the light UBO, the lit shaders' sets)");
}

void Renderer::upload_lighting() {
    const map::ScmapLighting& l = lighting_;
    LightUboData d{};
    for (int i = 0; i < 3; ++i) {
        // As the map stores it: Moho hands SunDirection to the shaders
        // unnormalized (retail maps' have unit length).
        d.sun_direction[i] = l.sun_direction[i];
        d.sun_color[i] = l.sun_color[i];
        d.sun_ambience[i] = l.sun_ambience[i];
        d.shadow_fill[i] = l.shadow_fill[i];
    }
    d.sun_color[3] = l.multiplier;
    // The terrain technique: 0 TTerrain, 1 TTerrainXP, 2 TTerrainGlow
    d.sun_ambience[3] = terrain_xp_ ? 1.0f : (terrain_glow_ ? 2.0f : 0.0f);
    for (int i = 0; i < 4; ++i) d.specular[i] = l.specular[i];
    for (u32 f = 0; f < FRAMES_IN_FLIGHT; ++f) {
        if (!light_ubo_mapped_[f]) continue;
        // The matrix (the first 64 bytes) and the shadow's state (the last
        // 16) are the shadow pass's, each frame.
        std::memcpy(static_cast<char*>(light_ubo_mapped_[f]) +
                        offsetof(LightUboData, sun_direction),
                    reinterpret_cast<const char*>(&d) + offsetof(LightUboData, sun_direction),
                    offsetof(LightUboData, shadow) - offsetof(LightUboData, sun_direction));
    }
}

void Renderer::record_shadow_pass(u32 fi, const std::array<f32, 16>& view_proj) {
    if (!shadow_map_.ready() || !light_ubo_mapped_[fi]) return;
    PROFILE_ZONE("Render::shadow_pass");

    // Moho's light camera (Shadow::PrepareLightCamera, M210c): none at
    // shadow fidelity 0, past ren_ShadowLOD, or with no terrain in view;
    // without one nothing is shadowed.
    std::optional<std::array<f32, 16>> light;
    if (shadow_fidelity() > 0 && height_bounds_) {
        ShadowView sight;
        sight.view_proj = view_proj;
        sight.forward = camera_.direction();
        sight.zoom = camera_.zoom();
        sight.sun = {lighting_.sun_direction[0], lighting_.sun_direction[1],
                     lighting_.sun_direction[2]};
        light = shadow_camera(sight, *height_bounds_);
    }
    shadow_camera_valid_ = light.has_value();
    static constexpr std::array<f32, 16> kIdentity = {1, 0, 0, 0, 0, 1, 0, 0,
                                                      0, 0, 1, 0, 0, 0, 0, 1};
    const std::array<f32, 16> light_vp = light ? *light : kIdentity;
    auto* ubo = static_cast<char*>(light_ubo_mapped_[fi]);
    std::memcpy(ubo, light_vp.data(), sizeof(f32) * 16);
    const std::array<f32, 4> state = {shadow_camera_valid_ ? 1.0f : 0.0f, kShadowBias,
                                      static_cast<f32>(SHADOW_MAP_SIZE), 0.0f};
    std::memcpy(ubo + offsetof(LightUboData, shadow), state.data(), sizeof(state));

    // The map: the terrain (R its depth over 128, G 1), then the meshes
    // and the placeholder cubes (R their depth, G 0).
    shadow_map_.begin_map(cmd_buf_[fi]);
    if (light) {
        ShadowCasters::Frame casters;
        casters.terrain = &terrain_mesh_;
        casters.units = &unit_renderer_;
        casters.bones = bone_ds_[fi];
        casters.albedo_fallback = texture_cache_.fallback_descriptor();
        casters.lane = static_cast<u32>(fidelity());
        casters.strategic = strategic_icon_renderer_.is_strategic_zoom();
        shadow_casters_.record(cmd_buf_[fi], light_vp, casters);
    }
    vkCmdEndRenderPass(cmd_buf_[fi]);

    // The terrain's mask, B: Moho's blur, or a copy of the map's G.
    shadow_map_.record_mask(cmd_buf_[fi], video_options_.shadow_blur);
}

void Renderer::create_pipelines() {
    // Compile shaders from embedded GLSL
    auto tv = compile_glsl(device_, shaders::terrain_vert, "terrain.vert", true);
    auto tf = compile_glsl(device_, shaders::terrain_frag(), "terrain.frag", false);
    auto uv = compile_glsl(device_, shaders::unit_vert, "unit.vert", true);
    auto uf = compile_glsl(device_, shaders::unit_frag, "unit.frag", false);
    auto mv = compile_glsl(device_, shaders::mesh_vert, "mesh.vert", true);
    auto mf = compile_glsl(device_, shaders::mesh_frag, "mesh.frag", false);
    auto dv = compile_glsl(device_, shaders::decal_lit_vert, "decal_lit.vert", true);
    auto df = compile_glsl(device_, shaders::decal_lit_frag(), "decal_lit.frag", false);

    // Abort if any shader failed to compile
    if (!tv || !tf || !uv || !uf || !mv || !mf || !dv || !df) {
        spdlog::error("One or more shaders failed to compile");
        auto safe_destroy = [&](VkShaderModule m) {
            if (m) vkDestroyShaderModule(device_, m, nullptr);
        };
        safe_destroy(tv); safe_destroy(tf); safe_destroy(uv);
        safe_destroy(uf);
        safe_destroy(mv); safe_destroy(mf);
        safe_destroy(dv); safe_destroy(df);
        return;
    }

    // --- Terrain pipeline ---
    {
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(TerrainVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::array<VkVertexInputAttributeDescription, 2> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};                  // position
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(f32) * 3};    // normal

        // Push constant: mat4 viewProj(64) + mapW(4) + mapH(4) + pad(8) + eye(12) = 92B
        const auto terrain_builder = [&](VkShaderModule frag) {
            PipelineBuilder b;
            b.set_shaders(tv, frag)
                .set_vertex_input(&binding, 1, attrs.data(), static_cast<u32>(attrs.size()))
                .set_depth_test(true, true)
                .set_cull_mode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .set_push_constant(92, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                .set_descriptor_set_layout(terrain_tex_ds_layout_) // set=0: terrain textures
                .add_descriptor_set_layout(shadow_ds_layout_);     // set=1: shadow
            return b;
        };
        terrain_pipeline_ =
            terrain_builder(tf).build(device_, frame_.scene_pass(), &terrain_layout_);
        // The low fidelity terrain (M212h; LowFidelityTerrain, then its
        // lighting): the same vertices, sets and push block.
        VkShaderModule low =
            compile_glsl(device_, shaders::terrain_low_frag(), "terrain_low.frag", false);
        if (low) {
            terrain_low_pipeline_ =
                terrain_builder(low).build(device_, frame_.scene_pass(), &terrain_low_layout_);
            vkDestroyShaderModule(device_, low, nullptr);
        }
        VkShaderModule skirt =
            compile_glsl(device_, shaders::terrain_skirt_frag, "terrain_skirt.frag", false);
        if (skirt) {
            terrain_skirt_pipeline_ =
                terrain_builder(skirt)
                    .set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                    .set_color_write_mask(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT)
                    .build(device_, frame_.scene_pass(), &terrain_skirt_layout_);
            vkDestroyShaderModule(device_, skirt, nullptr);
        }
    }

    // --- The terrain in the normal pass (M212e): its normals into the
    // normal target, depth-tested and written, as TTerrainNormals draws. ---
    {
        VkShaderModule nf =
            compile_glsl(device_, shaders::terrain_normal_frag(), "terrain_normal.frag", false);
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(TerrainVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        std::array<VkVertexInputAttributeDescription, 2> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};               // position
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(f32) * 3}; // normal
        if (nf)
            terrain_normal_pipeline_ =
                PipelineBuilder()
                    .set_shaders(tv, nf)
                    .set_vertex_input(&binding, 1, attrs.data(), static_cast<u32>(attrs.size()))
                    .set_depth_test(true, true)
                    .set_cull_mode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                    .set_push_constant(92,
                                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                    .set_descriptor_set_layout(terrain_tex_ds_layout_) // set=0: terrain textures
                    .add_descriptor_set_layout(shadow_ds_layout_)      // set=1: its light
                    .build(device_, frame_.scene_pass(), &terrain_normal_layout_);
        if (nf) vkDestroyShaderModule(device_, nf, nullptr);
    }

    // --- Unit pipeline (instanced cubes — fallback) ---
    {
        std::array<VkVertexInputBindingDescription, 2> bindings{};
        // Binding 0: per-vertex cube data
        bindings[0].binding = 0;
        bindings[0].stride = sizeof(f32) * 6; // pos + normal
        bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        // Binding 1: per-instance data
        bindings[1].binding = 1;
        bindings[1].stride = sizeof(CubeInstance);
        bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        std::array<VkVertexInputAttributeDescription, 5> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};                  // position
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(f32) * 3};    // normal
        attrs[2] = {2, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CubeInstance, x)};   // instancePos
        attrs[3] = {3, 1, VK_FORMAT_R32_SFLOAT,       offsetof(CubeInstance, scale)};// scale
        attrs[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CubeInstance, r)}; // color

        unit_pipeline_ =
            PipelineBuilder()
                .set_shaders(uv, uf)
                .set_vertex_input(bindings.data(), static_cast<u32>(bindings.size()), attrs.data(),
                                  static_cast<u32>(attrs.size()))
                .set_depth_test(true, true)
                .set_blend(true)
                .set_cull_mode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .set_push_constant(sizeof(f32) * 19, // viewProj(64) + eye(12) = 76B
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                .set_descriptor_set_layout(shadow_ds_layout_) // set=0: shadow
                .set_color_write_mask(kColorOnly)             // the glow in alpha stays (M211e)
                .build(device_, frame_.scene_pass(), &unit_layout_);
    }

    // --- Mesh pipeline (real SCM meshes, GPU skinning, per-instance model matrix + texture) ---
    {
        std::array<VkVertexInputBindingDescription, 2> bindings{};
        // Binding 0: per-vertex mesh data (pos + normal + UV + bone_indices + bone_weights +
        // tangent + binormal = 76 bytes)
        bindings[0].binding = 0;
        bindings[0].stride = static_cast<u32>(sizeof(sim::SCMMesh::Vertex));
        bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        // Binding 1: per-instance data (mat4 model + vec4 color + colour lookup + time +
        // parameter = 92 bytes)
        bindings[1].binding = 1;
        bindings[1].stride = sizeof(MeshInstance);
        bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        // 15 attributes: pos(0), normal(1), uv(2), model col0-3(3-6), color(7), bone_indices(8),
        // bone_weights(9), tangent(10), binormal(11), colour lookup(12), instance time(13),
        // parameter(14)
        std::array<VkVertexInputAttributeDescription, 15> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};                              // position
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(f32) * 3};                // normal
        attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(f32) * 6};                   // UV
        attrs[3] = {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, model) + 0};
        attrs[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, model) + sizeof(f32) * 4};
        attrs[5] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, model) + sizeof(f32) * 8};
        attrs[6] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, model) + sizeof(f32) * 12};
        attrs[7] = {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, r)};   // color
        attrs[8] = {8, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(sim::SCMMesh::Vertex, bone_indices)};    // bone_indices
        attrs[9] = {9, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(sim::SCMMesh::Vertex, bone_weights)}; // bone_weights
        attrs[10] = {10, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(sim::SCMMesh::Vertex, tx)};  // tangent
        attrs[11] = {11, 0, VK_FORMAT_R32G32B32_SFLOAT,
                     offsetof(sim::SCMMesh::Vertex, bx)}; // binormal
        attrs[12] = {12, 1, VK_FORMAT_R32_SFLOAT, offsetof(MeshInstance, color_lookup)};
        attrs[13] = {13, 1, VK_FORMAT_R32_SFLOAT, offsetof(MeshInstance, shader_time)};
        attrs[14] = {14, 1, VK_FORMAT_R32_SFLOAT, offsetof(MeshInstance, parameter)};

        // Push constant: mat4 viewProj (64B) + uint boneBase (4B) + uint bonesPerInst (4B) + vec3
        // eye (12B) + uint technique (4B, M211b) + uint pass + float time (8B, M211f) + uint
        // mirrored + float surface (8B, M213b) = 104B
        // Opaque meshes write their glow to alpha (M211e); fading ones blend
        // by their alpha and write colour only; the build overlays that
        // write alpha blend it too, as D3D9 does (M211f); UEF's build cube
        // leaves depth unwritten (M211g). The layouts match.
        // The order marks (CommandFeedback) blend colour with no depth test.
        enum class Blend { Opaque, Fade, Overlay, FadeNoDepthWrite, Feedback };
        const auto build_mesh = [&](Blend blend, VkPipelineLayout* layout) {
            const bool colour_only = blend == Blend::Fade || blend == Blend::FadeNoDepthWrite ||
                                     blend == Blend::Feedback;
            return PipelineBuilder()
                .set_shaders(mv, mf)
                .set_vertex_input(bindings.data(), static_cast<u32>(bindings.size()), attrs.data(),
                                  static_cast<u32>(attrs.size()))
                .set_depth_test(blend != Blend::Feedback,
                                blend != Blend::FadeNoDepthWrite && blend != Blend::Feedback)
                .set_blend(blend != Blend::Opaque)
                .set_alpha_blend(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA)
                .set_color_write_mask(colour_only ? kColorOnly : kColorAndGlow)
                .set_cull_mode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .set_push_constant(sizeof(f32) * 16 + sizeof(u32) * 2 + sizeof(f32) * 3 +
                                       sizeof(u32) * 2 + sizeof(f32) + sizeof(u32) + sizeof(f32) +
                                       sizeof(u32) * 2,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                .set_descriptor_set_layout(texture_ds_layout_) // set=0: albedo
                .add_descriptor_set_layout(bone_ds_layout_)    // set=1: bone SSBO
                .add_descriptor_set_layout(texture_ds_layout_) // set=2: specteam
                .add_descriptor_set_layout(texture_ds_layout_) // set=3: normal map
                .add_descriptor_set_layout(shadow_ds_layout_)  // set=4: shadow
                .add_descriptor_set_layout(texture_ds_layout_) // set=5: lookup
                .add_descriptor_set_layout(texture_ds_layout_) // set=6: secondary
                .build(device_, frame_.scene_pass(), layout);
        };
        mesh_pipeline_ = build_mesh(Blend::Opaque, &mesh_layout_);
        mesh_fade_pipeline_ = build_mesh(Blend::Fade, &mesh_fade_layout_);
        mesh_overlay_pipeline_ = build_mesh(Blend::Overlay, &mesh_overlay_layout_);
        mesh_cube_pipeline_ = build_mesh(Blend::FadeNoDepthWrite, &mesh_cube_layout_);
        mesh_feedback_pipeline_ = build_mesh(Blend::Feedback, &mesh_feedback_layout_);

        // The shields' (M211k): their own shaders, the mesh's input, push
        // block and sets, and mesh.fx's states. Each depth-tests; only the
        // fill writes depth (and nothing else). Blending RGBA blends the
        // glow in alpha by the colour's factors, as D3D9 does.
        VkShaderModule sv = compile_glsl(device_, shaders::shield_vert(), "shield.vert", true);
        VkShaderModule sf = compile_glsl(device_, shaders::shield_frag(), "shield.frag", false);
        const auto build_shield = [&](ShieldState state, VkPipelineLayout* layout) {
            const bool added = state == ShieldState::AddRGB || state == ShieldState::AddRGBA;
            const VkBlendFactor dst =
                added ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            VkColorComponentFlags mask = kColorAndGlow;
            if (state == ShieldState::AddRGB) mask = kColorOnly;
            else if (state == ShieldState::Fill) mask = 0;
            return PipelineBuilder()
                .set_shaders(sv, sf)
                .set_vertex_input(bindings.data(), static_cast<u32>(bindings.size()), attrs.data(),
                                  static_cast<u32>(attrs.size()))
                .set_depth_test(true,
                                state == ShieldState::Fill || state == ShieldState::BlendDepthWrite)
                .set_blend(state != ShieldState::Fill)
                .set_color_blend(VK_BLEND_FACTOR_SRC_ALPHA, dst)
                .set_alpha_blend(VK_BLEND_FACTOR_SRC_ALPHA, dst)
                .set_color_write_mask(mask)
                .set_cull_mode(state == ShieldState::BlendUnculled ? VK_CULL_MODE_NONE
                                                                   : VK_CULL_MODE_BACK_BIT,
                               VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .set_push_constant(sizeof(f32) * 16 + sizeof(u32) * 2 + sizeof(f32) * 3 +
                                       sizeof(u32) * 2 + sizeof(f32) + sizeof(u32) + sizeof(f32) +
                                       sizeof(u32) * 2,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                .set_descriptor_set_layout(texture_ds_layout_) // set=0: albedo
                .add_descriptor_set_layout(bone_ds_layout_)    // set=1: bone SSBO
                .add_descriptor_set_layout(texture_ds_layout_) // set=2: specular
                .add_descriptor_set_layout(texture_ds_layout_) // set=3: normal map
                .add_descriptor_set_layout(shadow_ds_layout_)  // set=4: light, environment
                .add_descriptor_set_layout(texture_ds_layout_) // set=5: lookup
                .add_descriptor_set_layout(texture_ds_layout_) // set=6: secondary
                .build(device_, frame_.scene_pass(), layout);
        };
        for (u32 i = 0; i < kShieldStates; ++i)
            shield_pipelines_[i] = build_shield(static_cast<ShieldState>(i), &shield_layouts_[i]);
        vkDestroyShaderModule(device_, sv, nullptr);
        vkDestroyShaderModule(device_, sf, nullptr);
    }

    // --- The map's decals (M212b): the terrain's vertices, lit as the
    // terrain is, blended over it (TDecals / TDecalsXP: SrcAlpha /
    // InvSrcAlpha, RGB, depth LessEqual unwritten, FA's bias in the shader).
    {
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(TerrainVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        std::array<VkVertexInputAttributeDescription, 2> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};               // position
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(f32) * 3}; // normal

        // Push constant: viewProj (64) + u, v, map/alpha/XP, eye (4 vec4s) = 128B.
        // Every decal technique's pipeline takes these sets and this block.
        const auto decal_builder = [&](VkShaderModule frag, VkShaderModule vert = VK_NULL_HANDLE) {
            PipelineBuilder b;
            b.set_shaders(vert ? vert : dv, frag)
                .set_vertex_input(&binding, 1, attrs.data(), static_cast<u32>(attrs.size()))
                .set_depth_test(true, false)
                .set_cull_mode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .set_push_constant(sizeof(f32) * 32,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                .set_descriptor_set_layout(terrain_tex_ds_layout_) // set=0: the terrain's
                .add_descriptor_set_layout(shadow_ds_layout_)      // set=1: shadow and light
                .add_descriptor_set_layout(texture_ds_layout_)     // set=2: albedo
                .add_descriptor_set_layout(texture_ds_layout_)     // set=3: specular
                .add_descriptor_set_layout(texture_ds_layout_);    // set=4: the mask
            return b;
        };
        decal_pipeline_ = decal_builder(df)
                              .set_blend(true)
                              .set_color_write_mask(kColorOnly) // the glow in alpha stays (M211e)
                              .build(device_, frame_.scene_pass(), &decal_layout_);
        // The glowing decals (M212d; TDecalsGlow): One/One into alpha alone,
        // the frame's glow. The glow masks (TDecalGlowMask): no blending,
        // colour and glow both written.
        VkShaderModule glow =
            compile_glsl(device_, shaders::decal_glow_frag(), "decal_glow.frag", false);
        VkShaderModule glow_mask =
            compile_glsl(device_, shaders::decal_glow_mask_frag(), "decal_glow_mask.frag", false);
        if (glow)
            decal_glow_pipeline_ = decal_builder(glow)
                                       .set_blend(true)
                                       .set_alpha_blend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE)
                                       .set_color_write_mask(VK_COLOR_COMPONENT_A_BIT)
                                       .build(device_, frame_.scene_pass(), &decal_glow_layout_);
        if (glow_mask)
            decal_glow_mask_pipeline_ =
                decal_builder(glow_mask)
                    .set_color_write_mask(kColorAndGlow)
                    .build(device_, frame_.scene_pass(), &decal_glow_mask_layout_);
        // The normal decals (M212e; TDecalsNormals): into the normal
        // target's RG, SrcAlpha / InvSrcAlpha.
        VkShaderModule normals =
            compile_glsl(device_, shaders::decal_normal_frag(), "decal_normal.frag", false);
        if (normals)
            decal_normal_pipeline_ =
                decal_builder(normals)
                    .set_blend(true)
                    .set_color_write_mask(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT)
                    .build(device_, frame_.scene_pass(), &decal_normal_layout_);
        // The water's albedo decals (M212g; TDecalsWaterAlbedo): on its
        // surface, SrcAlpha / InvSrcAlpha into RGB.
        VkShaderModule water_vert =
            compile_glsl(device_, shaders::decal_water_vert, "decal_water.vert", true);
        VkShaderModule water_frag =
            compile_glsl(device_, shaders::decal_water_frag, "decal_water.frag", false);
        if (water_vert && water_frag)
            decal_water_pipeline_ = decal_builder(water_frag, water_vert)
                                        .set_blend(true)
                                        .set_color_write_mask(kColorOnly)
                                        .build(device_, frame_.scene_pass(), &decal_water_layout_);
        if (glow) vkDestroyShaderModule(device_, glow, nullptr);
        if (glow_mask) vkDestroyShaderModule(device_, glow_mask, nullptr);
        if (normals) vkDestroyShaderModule(device_, normals, nullptr);
        if (water_vert) vkDestroyShaderModule(device_, water_vert, nullptr);
        if (water_frag) vkDestroyShaderModule(device_, water_frag, nullptr);
    }

    // --- UI 2D pipeline (screen-space textured quads, no depth, alpha blend) ---
    auto uiv = compile_glsl(device_, shaders::ui_vert, "ui.vert", true);
    auto uif = compile_glsl(device_, shaders::ui_frag, "ui.frag", false);
    if (uiv && uif) {
        // Only per-instance input (no per-vertex — quad generated from gl_VertexIndex)
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(UIInstance);
        binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        std::array<VkVertexInputAttributeDescription, 3> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};                   // rect (x,y,w,h)
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(f32) * 4};     // uvRect
        attrs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(f32) * 8};     // color

        ui_pipeline_ = PipelineBuilder()
            .set_shaders(uiv, uif)
            .set_vertex_input(&binding, 1, attrs.data(),
                              static_cast<u32>(attrs.size()))
            .set_depth_test(false, false)
            .set_blend(true)
            .set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .set_push_constant(sizeof(f32) * 2,
                               VK_SHADER_STAGE_VERTEX_BIT)
            .set_descriptor_set_layout(texture_ds_layout_)
            .build(device_, render_pass_, &ui_layout_);
    }

    // Destroy shader modules (already compiled into pipelines)
    vkDestroyShaderModule(device_, tv, nullptr);
    vkDestroyShaderModule(device_, tf, nullptr);
    vkDestroyShaderModule(device_, uv, nullptr);
    vkDestroyShaderModule(device_, uf, nullptr);
    vkDestroyShaderModule(device_, mv, nullptr);
    vkDestroyShaderModule(device_, mf, nullptr);
    vkDestroyShaderModule(device_, dv, nullptr);
    vkDestroyShaderModule(device_, df, nullptr);
    if (uiv) vkDestroyShaderModule(device_, uiv, nullptr);
    if (uif) vkDestroyShaderModule(device_, uif, nullptr);
}


void Renderer::create_frame_targets() {
    u32 w = window_width_;
    u32 h = window_height_;
    frame_.create(device_, allocator_, w, h, depth_format_, depth_image_.view);

    bloom_.create(device_, allocator_, w, h, frame_.scene_color().view, texture_ds_layout_,
                  texture_sampler_);

    water_renderer_.set_frame_images(
        {.refraction = frame_.refraction().view, .reflection = frame_.reflection().view});
    bind_normal_target(); // the terrain reads the new one (M212e)
    // The refracting particles bend the same copy, made again for them.
    particle_renderer_.set_background(frame_.refraction().view);
    spdlog::info("Frame targets created ({}x{})", w, h);
}

void Renderer::destroy_frame_targets() {
    bloom_.destroy();

    frame_.destroy();
}

void Renderer::destroy_decal_buffers() {
    const auto destroy = [&](AllocatedBuffer& b) {
        if (b.buffer) vmaDestroyBuffer(allocator_, b.buffer, b.allocation);
        b = {};
    };
    destroy(decal_indices_);
}

void Renderer::add_command_feedback_blip(FeedbackBlipSpec spec) {
    // Made at the session's game tick, as material.x keeps it (the shader
    // time's whole ticks, already wrapped at 36000).
    feedback_blips_.add(std::move(spec), std::floor(unit_renderer_.shader_time()));
}

void Renderer::inject_feedback_blips(lua_State* L, const std::vector<WorldMeshDraw>& world_meshes) {
    if (feedback_blips_.blips().empty() && world_meshes.empty()) {
        return;
    }
    sim::Vector3 eye;
    camera_.eye_position(eye.x, eye.y, eye.z);
    sim::Vector3 forward{camera_.focus_x() - eye.x, camera_.focus_y() - eye.y,
                         camera_.focus_z() - eye.z};
    const f32 length =
        std::sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
    if (length <= 0.0f) {
        return;
    }
    forward = {forward.x / length, forward.y / length, forward.z / length};
    const auto inject = [&](const FeedbackBlipSpec& spec, f32 created_tick, f32 parameter,
                            f32 lod_cutoff) {
        // MeshName at its UniformScale, else the blueprint's LOD 0 mesh at
        // the blueprint's; the albedo and technique are the blip's either way.
        const MeshTechnique technique = osc::renderer::mesh_technique(spec.shader_name);
        const GPUMesh* mesh = nullptr;
        f32 scale = spec.uniform_scale;
        if (!spec.mesh_name.empty()) {
            mesh = mesh_cache_.get_file(spec.mesh_name, spec.texture_name, technique);
        } else if (!spec.blueprint_id.empty() && L) {
            mesh = spec.texture_name.empty()
                       ? mesh_cache_.get(spec.blueprint_id, L)
                       : mesh_cache_.get_file(mesh_cache_.mesh_file(spec.blueprint_id, L),
                                              spec.texture_name, technique);
            scale = mesh_cache_.blueprint_scale(spec.blueprint_id, L);
        }
        if (!mesh) {
            return;
        }
        // Its one LOD, to a metric of 1000; CommandFeedbackVS grows it with
        // the metric so it reads the same size from afar.
        const f32 lod = lod_metric(spec.position, eye, forward, camera_.fov());
        if (lod > lod_cutoff) {
            return;
        }
        if (is_feedback_technique(mesh->technique)) {
            scale *= feedback_distance_scale(lod);
        }
        MeshInstance inst{};
        const auto model = feedback_model(spec.position, scale);
        std::copy(model.begin(), model.end(), inst.model);
        inst.r = inst.g = inst.b = inst.a = 1.0f;
        inst.shader_time = created_tick;
        inst.parameter = parameter;
        unit_renderer_.inject_mesh(mesh, inst, &texture_cache_);
    };
    for (const FeedbackBlip& blip : feedback_blips_.blips()) {
        // The lifetime parameter, in ticks (material.y, PARAM_LIFETIME)
        inject(blip.spec, blip.created_tick, blip.spec.duration * 10.0f, kFeedbackLodCutoff);
    }
    for (const WorldMeshDraw& world_mesh : world_meshes) {
        inject(world_mesh.spec, world_mesh.created_tick, world_mesh.lifetime,
               world_mesh.lod_cutoff);
    }
}

void Renderer::clear_scene() {
    vkDeviceWaitIdle(device_);
    feedback_blips_.clear();         // the old world's order marks
    range_blueprints_.clear();       // read again from the next UI state
    minimap_renderer_.begin_frame(); // no minimap (or its clicks) until drawn again
    recon_.clear();                  // a new world: nothing seen of it yet
    playable_rect_.clear();          // nor hidden

    terrain_mesh_.destroy(device_, allocator_);
    unit_renderer_.destroy(device_, allocator_);
    water_renderer_.clear();
    sky_renderer_.clear();
    fog_renderer_.destroy(device_, allocator_);

    if (bone_ds_pool_) {
        vkDestroyDescriptorPool(device_, bone_ds_pool_, nullptr);
        bone_ds_pool_ = VK_NULL_HANDLE;
        for (auto& ds : bone_ds_) ds = VK_NULL_HANDLE;
    }
    if (terrain_tex_ds_pool_) {
        vkDestroyDescriptorPool(device_, terrain_tex_ds_pool_, nullptr);
        terrain_tex_ds_pool_ = VK_NULL_HANDLE;
        terrain_tex_ds_ = VK_NULL_HANDLE;
    }

    stored_decals_.clear();
    decal_frames_.clear();
    decal_mask_ds_ = VK_NULL_HANDLE;
    // build_scene makes them again for the next map: a game started from
    // another leaked the last one's otherwise.
    destroy_decal_buffers();
    // Textures made for this map, cached by name: the next map's would
    // otherwise be this one's. The minimap lets go of its first, so a reload
    // that fails before the next map is built draws no minimap rather than a
    // destroyed texture.
    minimap_renderer_.forget_terrain();
    if (caches_initialized_) {
        for (const char* key : {"__terrain_blend0", "__terrain_blend1", "__terrain_normal_maps",
                                "__osc_minimap_terrain", "__water_map", "__water_fresnel"})
            texture_cache_.evict(key);
    }
    // The waves' particles point into the wave system's blueprints (M213c):
    // the particles go first.
    particle_system_.clear();
    wave_system_.clear();
    runtime_decals_.clear();
    emitter_bp_cache_.clear();
    terrain_ = nullptr;
    height_bounds_.reset();
    shadow_camera_valid_ = false;
    beam_bp_cache_.clear();
    // The trails' segments point into their blueprint cache (M214b).
    trail_renderer_.clear();
    trail_bp_cache_.clear();
    strategic_icon_renderer_.forget_blueprints(); // likewise the icons' (M215c)

    terrain_map_width_ = 0;
    terrain_map_height_ = 0;
    destroy_terrain_strata_ubo();
    camera_.set_ground(nullptr, false, 0.0f);
    ground_.reset();
    camera_view_ = sim::FrameView{}; // the old world's entities are gone
}

void Renderer::create_terrain_strata_ubo(const std::vector<map::StratumInfo>& strata,
                                         u32 normal_tile_w, u32 normal_tile_h) {
    destroy_terrain_strata_ubo();
    // A stratum's texture repeats every `size` world units (FA's tile is the
    // map's size over it). An unset or zero size stands at 1.
    TerrainStrataData d{};
    const auto size = [](f32 v) { return v > 0.0f ? v : 1.0f; };
    for (size_t i = 0; i < 10; ++i) {
        const bool have = i < strata.size();
        d.albedo_size[i] = size(have ? strata[i].albedo_scale : 1.0f);
        if (i < 9) d.normal_size[i] = size(have ? strata[i].normal_scale : 1.0f);
    }
    // normalSize8.yz: a normal-map tile's size, in texels a world unit each
    // (M212e).
    d.normal_size[9] = static_cast<f32>(std::max(1u, normal_tile_w));
    d.normal_size[10] = static_cast<f32>(std::max(1u, normal_tile_h));

    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = sizeof(d);
    ci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(allocator_, &ci, &alloc_ci, &terrain_strata_ubo_.buffer,
                             &terrain_strata_ubo_.allocation, &info));
    std::memcpy(info.pMappedData, &d, sizeof(d));
    vmaFlushAllocation(allocator_, terrain_strata_ubo_.allocation, 0, VK_WHOLE_SIZE);
}

void Renderer::destroy_terrain_strata_ubo() {
    if (terrain_strata_ubo_.buffer) {
        vmaDestroyBuffer(allocator_, terrain_strata_ubo_.buffer, terrain_strata_ubo_.allocation);
        terrain_strata_ubo_ = {};
    }
}

void Renderer::build_scene(const map::Terrain* terrain, blueprints::BlueprintStore* store,
                           const std::vector<std::string>& preload,
                           vfs::VirtualFileSystem* vfs, lua_State* L) {
    vfs_ = vfs;
    emitter_bp_cache_.set_vfs(vfs);
    beam_bp_cache_.set_vfs(vfs);
    trail_bp_cache_.set_vfs(vfs);
    terrain_ = terrain; // the water particles snap to (M214c)
    if (!terrain) {
        spdlog::warn("No terrain loaded — skipping scene build");
        return;
    }

    // The ground the camera's focus sits on: a copy, as the sim's terrain
    // is replaced on a reload.
    ground_ = terrain->heightmap();
    // The range overlays' volumes span the map's heights
    {
        const map::Heightmap& h = terrain->heightmap();
        f32 lowest = h.max_height();
        for (u32 gz = 0; gz < h.grid_height(); ++gz)
            for (u32 gx = 0; gx < h.grid_width(); ++gx)
                lowest = std::min(lowest, h.get_height_at_grid(gx, gz));
        range_renderer_.set_heights(lowest, h.max_height());
    }
    camera_.set_ground(&*ground_, terrain->has_water(), terrain->water_elevation());
    // The light camera fits the terrain in view (M210c).
    height_bounds_ = std::make_unique<HeightBounds>(*ground_);

    terrain_mesh_.build(*terrain, device_, allocator_, cmd_pool_,
                        graphics_queue_);
    // The map's light (M210a).
    lighting_ = terrain->lighting();
    terrain_xp_ = terrain->environment().terrain_shader == "TTerrainXP";
    terrain_glow_ = terrain->environment().terrain_shader == "TTerrainGlow";
    terrain_time_.reset();
    upload_lighting();

    unit_renderer_.build(device_, allocator_, cmd_pool_, graphics_queue_);

    // Initialize mesh cache, texture cache, and preload meshes
    if (vfs && store) {
        if (!caches_initialized_) {
            texture_cache_.init(device_, allocator_, cmd_pool_, graphics_queue_,
                                texture_ds_layout_, texture_sampler_, vfs);
            font_cache_.init(device_, allocator_, cmd_pool_, graphics_queue_,
                             texture_ds_layout_, texture_sampler_, vfs);
            caches_initialized_ = true;
        }
        // Every scene's, not only the first's: a front end's caches
        // (init_ui_caches) have no mesh cache, and every game launched from
        // one drew its units as cubes. The meshes it has loaded stay.
        mesh_cache_.init(device_, allocator_, cmd_pool_, graphics_queue_, vfs, store);
        unit_renderer_.preload_meshes(preload, mesh_cache_, L);
        // Their strategic icons, as Moho loads a blueprint's with it (M215c).
        strategic_icon_renderer_.preload(preload, texture_cache_, L);
    }
    // The colour tables that pick each army's row of a lookup (M211c), and
    // an unidentified blip's colour (M215a)
    if (L) {
        sim::GameColors colors = sim::read_game_colors(L);
        recon_.set_unidentified_color(colors.unidentified_color);
        unit_renderer_.set_game_colors(std::move(colors));
    }
    // The cubes and lookups meshes shade with (M211a/b), once the texture
    // cache is up.
    bind_mesh_environment(terrain->environment());

    // Bone SSBO descriptor pool and per-frame sets
    create_bone_descriptors();

    // FA's water: its quad, water map, Fresnel table and textures (M213a),
    // before the terrain, which is tinted under it by the water map.
    water_renderer_.build(*terrain, texture_cache_);
    // The shoreline's wave generators, out of step from the start (M213c)
    wave_system_.load(terrain->waves(), wave_clock_);
    // The map's sky dome (M210b)
    sky_renderer_.build(*terrain, texture_cache_);

    // The map's normal maps (M212e), whose tile size the strata's block holds.
    const TerrainNormalMaps normal_maps = terrain_normal_maps(*terrain);

    // Load terrain stratum textures and create terrain descriptor set
    bind_terrain_strata(*terrain, normal_maps);

    // Init fog of war texture: a texel per cell of the vision grid
    init_fog_texture(*terrain);

    // The map's normal maps (M212e; binding 21), which the normal pass's
    // basis samples, and the normal target it draws (binding 26).
    bind_terrain_normal_maps(normal_maps);

    // The map's lit decals (M212b) and retail's decal mask (M212c).
    load_map_decals(*terrain);

    // Build minimap terrain texture
    minimap_renderer_.build_terrain_texture(*terrain, texture_cache_);

    // Build strategic icon atlas
    strategic_icon_renderer_.build_atlas(texture_cache_);

    // The view's size before the reset: the farthest zoom takes its aspect
    camera_.set_viewport(static_cast<f32>(window_width_), static_cast<f32>(window_height_));
    camera_.init(static_cast<f32>(terrain->map_width()),
                 static_cast<f32>(terrain->map_height()));

    spdlog::info("Scene built");
}

void Renderer::create_bone_descriptors() {
    if (!unit_renderer_.bone_ssbo_buffer(0) || !bone_ds_layout_) return;
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = FRAMES_IN_FLIGHT;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    VK_CHECK(vkCreateDescriptorPool(device_, &pool_ci, nullptr, &bone_ds_pool_));

    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = bone_ds_pool_;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &bone_ds_layout_;
        VK_CHECK(vkAllocateDescriptorSets(device_, &alloc_info, &bone_ds_[i]));
        write_bone_descriptor(i);
    }
}

void Renderer::init_fog_texture(const map::Terrain& terrain) {
    const u32 cell = map::intel_cell_size(map::IntelLayer::Vision);
    const u32 fog_w = std::max<u32>(static_cast<u32>(terrain.map_width()) / cell, 1);
    const u32 fog_h = std::max<u32>(static_cast<u32>(terrain.map_height()) / cell, 1);
    fog_renderer_.init(fog_w, fog_h, device_, allocator_, cmd_pool_, graphics_queue_);

    // Write fog texture to terrain descriptor set binding 20
    if (fog_renderer_.initialized() && terrain_tex_ds_) {
        VkDescriptorImageInfo fog_info{};
        fog_info.sampler = fog_renderer_.sampler();
        fog_info.imageView = fog_renderer_.image_view();
        fog_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet fog_write{};
        fog_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        fog_write.dstSet = terrain_tex_ds_;
        fog_write.dstBinding = 20;
        fog_write.descriptorCount = 1;
        fog_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        fog_write.pImageInfo = &fog_info;
        vkc::update_descriptor_sets(device_, 1, &fog_write, 0, nullptr);
    }
}

void Renderer::bind_terrain_normal_maps(const TerrainNormalMaps& normal_maps) {
    if (!terrain_tex_ds_) return;
    const GPUTexture* maps =
        normal_maps.dds.empty()
            ? texture_cache_.upload_rgba("__terrain_normal_maps", normal_maps.rgba.data(),
                                         normal_maps.width, normal_maps.height)
            : texture_cache_.get_raw("__terrain_normal_maps", normal_maps.dds);
    VkDescriptorImageInfo info{};
    info.sampler = water_renderer_.clamp_sampler();
    info.imageView = maps ? maps->image.view : texture_cache_.normal_fallback_view();
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = terrain_tex_ds_;
    write.dstBinding = 21;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkc::update_descriptor_sets(device_, 1, &write, 0, nullptr);
    bind_normal_target();
    spdlog::info("Normal maps: {}x{} in tiles of {}x{}{}", normal_maps.width, normal_maps.height,
                 normal_maps.tile_width, normal_maps.tile_height,
                 normal_maps.dds.empty() ? " (made from the heights)" : "");
}

void Renderer::load_map_decals(const map::Terrain& terrain) {
    if (!terrain.decals().empty() && decal_pipeline_) {
        std::vector<u32> indices;
        for (const map::DecalInfo& d : terrain.decals()) {
            const std::optional<DecalTechnique> technique = decal_technique(d.type);
            if (!technique) continue; // Water Mask and Water Normals: never drawn
            if (d.scale_x == 0.0f || d.scale_y == 0.0f || d.scale_z == 0.0f) continue;
            StoredDecal sd;
            sd.albedo_path = d.texture_path;
            sd.spec_path = d.texture2_path;
            sd.technique = *technique;
            sd.rotation_y = d.rotation_y;
            decal_texture_matrix(d, sd.u, sd.v);
            f32 min_x = 0, min_z = 0, max_x = 0, max_z = 0;
            decal_bounds(d, min_x, min_z, max_x, max_z);
            sd.mid_x = (min_x + max_x) * 0.5f;
            sd.mid_z = (min_z + max_z) * 0.5f;
            sd.radius = 0.5f * std::hypot(max_x - min_x, max_z - min_z);
            sd.cut_off_lod = d.cut_off_lod;
            sd.near_cut_off_lod = d.near_cut_off_lod;
            sd.first_index = static_cast<u32>(indices.size());
            terrain_mesh_.collect_indices(min_x, min_z, max_x, max_z, indices);
            sd.index_count = static_cast<u32>(indices.size()) - sd.first_index;
            if (sd.index_count == 0) continue;
            // Loaded with the map, as Moho loads a map's decal textures: the
            // first frame shows them, every frame of an animated one too.
            for (const std::string& frame : decal_frames(sd.albedo_path))
                (void)texture_cache_.get_blocking(frame);
            if (!sd.spec_path.empty())
                for (const std::string& frame : decal_frames(sd.spec_path))
                    (void)texture_cache_.get_blocking(frame);
            stored_decals_.push_back(std::move(sd));
        }
        if (!indices.empty())
            decal_indices_ =
                upload_buffer(device_, allocator_, cmd_pool_, graphics_queue_, indices.data(),
                              indices.size() * sizeof(u32), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        spdlog::info("Decals: {} lit, over {} of the terrain's indices", stored_decals_.size(),
                     indices.size());
    }
    // Retail's mask, every decal's: the map's and the runtime ones (M212c).
    if (decal_pipeline_) {
        const GPUTexture* mask = texture_cache_.get_blocking("/textures/engine/decalMask.dds");
        decal_mask_ds_ = mask ? mask->descriptor_set : texture_cache_.fallback_descriptor();
    }
}

void Renderer::bind_terrain_strata(const map::Terrain& terrain,
                                   const TerrainNormalMaps& normal_maps) {
    if (!terrain_tex_ds_layout_ || terrain.strata().empty() || !texture_cache_.fallback_view())
        return;
    // Store map dimensions and strata scales
    terrain_map_width_ = static_cast<f32>(terrain.map_width());
    terrain_map_height_ = static_cast<f32>(terrain.map_height());
    auto& strata = terrain.strata();
    // Strata 0-8 blend by the masks; stratum 9 (the upper) lies over
    // them by its own alpha.
    for (size_t i = 0; i < strata.size() && i < 10; i++) {
        spdlog::info("Terrain stratum {}: albedo='{}' ({:.1f}) normal='{}' ({:.1f})", i,
                     strata[i].albedo_path, strata[i].albedo_scale, strata[i].normal_path,
                     strata[i].normal_scale);
    }
    create_terrain_strata_ubo(strata, normal_maps.tile_width, normal_maps.tile_height);

    const TerrainStrataViews strata_views = terrain_strata_views(terrain);
    const auto& views = strata_views.views;
    const bool blend0 = strata_views.blend0;
    const bool blend1 = strata_views.blend1;
    VkImageView zero_view = texture_cache_.zero_fallback_view();

    // Create descriptor pool and set
    std::array<VkDescriptorPoolSize, 2> pool_sizes{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[0].descriptorCount = 26;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = static_cast<u32>(pool_sizes.size());
    pool_ci.pPoolSizes = pool_sizes.data();
    VK_CHECK(vkCreateDescriptorPool(device_, &pool_ci, nullptr, &terrain_tex_ds_pool_));

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = terrain_tex_ds_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &terrain_tex_ds_layout_;
    VK_CHECK(vkAllocateDescriptorSets(device_, &alloc_info, &terrain_tex_ds_));

    // Write all 20 image descriptors
    std::array<VkDescriptorImageInfo, 20> img_infos{};
    std::array<VkWriteDescriptorSet, 20> writes{};
    for (u32 i = 0; i < 20; i++) {
        img_infos[i].sampler = texture_sampler_;
        img_infos[i].imageView = views[i];
        img_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = terrain_tex_ds_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &img_infos[i];
    }
    vkc::update_descriptor_sets(device_, 20, writes.data(), 0, nullptr);

    // The upper stratum (binding 22); without one, a transparent texel
    // leaves the strata below as they are.
    {
        const GPUTexture* upper = strata.size() > 9 && !strata[9].albedo_path.empty()
                                      ? texture_cache_.get_blocking(strata[9].albedo_path)
                                      : nullptr;
        VkDescriptorImageInfo upper_info{};
        upper_info.sampler = texture_sampler_;
        upper_info.imageView = upper ? upper->image.view : zero_view;
        upper_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorBufferInfo strata_info{};
        strata_info.buffer = terrain_strata_ubo_.buffer;
        strata_info.offset = 0;
        strata_info.range = sizeof(TerrainStrataData);

        // Under the water, the map's water ramp by the water map's depth
        // (ApplyWaterColor; M213a); a map without water has neither.
        const bool wet = water_renderer_.has_water();
        VkDescriptorImageInfo ramp_info{};
        ramp_info.sampler = water_renderer_.clamp_sampler();
        ramp_info.imageView = wet ? water_renderer_.ramp_view() : zero_view;
        ramp_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo depth_info = ramp_info;
        depth_info.imageView = wet ? water_renderer_.water_map_view() : zero_view;

        std::array<VkWriteDescriptorSet, 4> more{};
        more[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        more[2].dstSet = terrain_tex_ds_;
        more[2].dstBinding = 24;
        more[2].descriptorCount = 1;
        more[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        more[2].pImageInfo = &ramp_info;
        more[3] = more[2];
        more[3].dstBinding = 25;
        more[3].pImageInfo = &depth_info;
        more[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        more[0].dstSet = terrain_tex_ds_;
        more[0].dstBinding = 22;
        more[0].descriptorCount = 1;
        more[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        more[0].pImageInfo = &upper_info;
        more[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        more[1].dstSet = terrain_tex_ds_;
        more[1].dstBinding = kTerrainStrataBinding;
        more[1].descriptorCount = 1;
        more[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        more[1].pBufferInfo = &strata_info;
        vkc::update_descriptor_sets(device_, static_cast<u32>(more.size()), more.data(), 0,
                                    nullptr);
    }

    spdlog::info("Terrain textures: {} strata loaded, blend0={}, blend1={}", strata.size(),
                 blend0 ? "OK" : "fallback", blend1 ? "OK" : "fallback");
}

Renderer::TerrainStrataViews Renderer::terrain_strata_views(const map::Terrain& terrain) {
    TerrainStrataViews out;
    auto& strata = terrain.strata();
    // Collect 20 image views:
    // [blend0, blend1, stratum0..8 albedo, stratum0..8 normal]
    std::array<VkImageView, 20> views{};

    // Blend maps from embedded DDS
    VkImageView white_view = texture_cache_.fallback_view();
    VkImageView zero_view = texture_cache_.zero_fallback_view();
    VkImageView normal_fb_view = texture_cache_.normal_fallback_view();

    // A stratum with no albedo texture must have no influence. FA ignores
    // such strata, and maps rely on it: SCMP_009 stores a copy of blend0
    // as blend1 while strata 5-8 are empty, which painted most of the
    // terrain in the black placeholder. Zero those weight channels.
    auto masked_blend = [&](const std::vector<char>& dds, size_t first_stratum) {
        std::vector<char> copy = dds;
        bool unused[4];
        for (size_t c = 0; c < 4; ++c) {
            const size_t s = first_stratum + c;
            unused[c] = s >= strata.size() || strata[s].albedo_path.empty();
        }
        zero_dds_channels(copy, unused);
        return copy;
    };
    auto* blend0 =
        terrain.blend_dds_0().empty()
            ? nullptr
            : texture_cache_.get_raw("__terrain_blend0", masked_blend(terrain.blend_dds_0(), 1));
    auto* blend1 =
        terrain.blend_dds_1().empty()
            ? nullptr
            : texture_cache_.get_raw("__terrain_blend1", masked_blend(terrain.blend_dds_1(), 5));

    views[0] = blend0 ? blend0->image.view : zero_view; // black = no blending
    views[1] = blend1 ? blend1->image.view : zero_view;

    spdlog::info("Terrain blend maps: blend0={} ({}B), blend1={} ({}B), map={}x{}",
                 blend0 ? "OK" : "NONE", terrain.blend_dds_0().size(), blend1 ? "OK" : "NONE",
                 terrain.blend_dds_1().size(), terrain.map_width(), terrain.map_height());

    // Stratum albedo textures (0-8) at bindings 2-10
    // Empty strata use black (zero) so blend weights don't add white.
    // The set is written once, so the textures must be loaded now: an
    // async get() of a first load returned nothing, and the terrain kept
    // the white fallback all game.
    for (size_t i = 0; i < 9; i++) {
        if (i < strata.size() && !strata[i].albedo_path.empty()) {
            auto* tex = texture_cache_.get_blocking(strata[i].albedo_path);
            views[2 + i] = tex ? tex->image.view : white_view;
        } else {
            views[2 + i] = zero_view; // black = no color contribution
        }
    }

    // Stratum normal map textures (0-8) at bindings 11-19
    for (size_t i = 0; i < 9; i++) {
        if (i < strata.size() && !strata[i].normal_path.empty()) {
            auto* tex = texture_cache_.get_blocking(strata[i].normal_path);
            views[11 + i] = tex ? tex->image.view : normal_fb_view;
        } else {
            views[11 + i] = normal_fb_view;
        }
    }
    out.views = views;
    out.blend0 = blend0 != nullptr;
    out.blend1 = blend1 != nullptr;
    return out;
}

void Renderer::render(const sim::FrameView& view, sim::WorldEvents& events,
                      const BuildGhost* ghost, lua_State* L,
                      ui::UIControlRegistry* ui_registry,
                      const std::unordered_set<u32>* selected_ids) {
    // The view's aspect, which the camera's farthest zoom and projection
    // take (its moves and basis run in poll_events); the world it follows
    camera_.set_viewport(static_cast<f32>(window_width_), static_cast<f32>(window_height_));
    camera_view_ = view;
    for (const auto& follow : events.camera_follows) {
        camera_.camera_follow(follow.source, follow.projectile, follow.timeout);
    }
    events.camera_follows.clear();
    camera_game_time_ =
        view.cur() ? (static_cast<f64>(view.cur()->tick) + view.alpha()) * 0.1 : 0.0;
    // FA's own game interface replaces the C++ HUD placeholders.
    {
        bool world_ui = false;
        if (L) {
            lua_pushstring(L, core::kWorldUiActiveKey);
            lua_rawget(L, LUA_REGISTRYINDEX);
            world_ui = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        }
        legacy_hud_active_ = legacy_hud_ || !world_ui;

        // The C++ HUD's intel rings; FA's UI draws its range overlays
        overlay_renderer_.set_intel_ring_types(
            legacy_hud_active_ ? kAllIntelRingTypes : std::unordered_set<std::string>{});
    }

    PROFILE_ZONE("Render::frame");
    // Select current frame's sync objects
    u32 fi = frame_index_;

    // This slot's last GPU work done, and a swapchain image (or the frame
    // is skipped: don't reset the fence, there's no submission)
    u32 image_index = 0;
    {
        PROFILE_ZONE("Render::gpu_wait");
        const auto wait_start = std::chrono::steady_clock::now();
        const bool ready = begin_frame_slot(fi, image_index);
        last_gpu_wait_ms_ =
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - wait_start)
                .count();
        if (!ready) return;
    }

    vkResetFences(device_, 1, &render_fence_[fi]);

    // Publish this frame's index to every sub-renderer BEFORE any of them
    // writes its per-frame buffers: fence[fi] was just waited on, so only
    // slot fi is free. (Setting it after the updates made them write into
    // the other slot while the GPU could still be reading it, and the draws
    // then showed data one frame old.)
    unit_renderer_.set_frame_index(fi);
    ui_renderer_.set_frame_index(fi);
    overlay_renderer_.set_frame_index(fi);
    minimap_renderer_.set_frame_index(fi);
    minimap_renderer_.begin_frame();
    strategic_icon_renderer_.set_frame_index(fi);
    hud_renderer_.set_frame_index(fi);
    selection_info_renderer_.set_frame_index(fi);
    fog_renderer_.set_frame_index(fi);
    profile_overlay_.set_frame_index(fi);

    // Camera shakes of the ticks since the last frame
    {
        if (!events.shakes.empty()) {
            f32 total_intensity = 0;
            for (const auto& ev : events.shakes) {
                f32 dx = camera_.target_x() - ev.x;
                f32 dz = camera_.target_z() - ev.z;
                f32 dist = std::sqrt(dx * dx + dz * dz);
                if (dist < ev.radius) {
                    f32 t = 1.0f - dist / ev.radius;
                    total_intensity += ev.min_shake + t * (ev.max_shake - ev.min_shake);
                }
            }
            camera_.apply_shake(total_intensity);
            events.shakes.clear();
        }
    }

    // Finalize any async texture loads that completed this frame
    texture_cache_.flush_uploads(4);

    // View-projection matrix (computed early for frustum culling)
    f32 aspect = static_cast<f32>(window_width_) /
                 static_cast<f32>(window_height_);
    auto vp = camera_.view_proj(aspect);
    Frustum frustum(vp);

    update_frame_scene(fi, vp, frustum, view, events, ghost, L, ui_registry, selected_ids);

    // Record command buffer
    begin_commands(fi);
    gpu_queries_.begin(cmd_buf_[fi], fi, frame_sequence_ + 1);
    mesh_draws_.fill(0); // this frame's, for tests (M211k)

    // Upload fog of war texture (barriers + copy, before any render pass)
    if (fog_renderer_.initialized()) {
        fog_renderer_.record_upload(cmd_buf_[fi]);
    }
    // The movies' new frames, likewise.
    movie_textures_.record(cmd_buf_[fi]);

    // ==================== SHADOW PASS ====================
    record_shadow_pass(fi, vp);

    // ==================== NORMALS ====================
    collect_frame_decals(frustum,
                         view.cur() ? static_cast<f32>(view.cur()->tick) + view.alpha() : 0.0f);
    record_normal_pass(fi, vp);

    // ==================== REFLECTION ====================
    record_reflection_pass(fi, vp);

    // ==================== MAIN PASS ====================
    PROFILE_ZONE("Render::main_pass");

    bool do_bloom = bloom_enabled_ && bloom_.ready();

    record_main_pass(fi, vp);

    // ==================== COMPOSITE + BLOOM ====================
    // Scene always renders to offscreen HDR. End scene pass, optionally run
    // bloom bright extract + blur, then composite scene (+bloom) onto swapchain.
    vkCmdEndRenderPass(cmd_buf_[fi]);
    const bool scene_capturing = record_scene_capture(cmd_buf_[fi]);

    if (do_bloom)
        bloom_.record(cmd_buf_[fi], kBloomGlowCopyScale, lighting_.bloom, kBloomBlurKernelScale,
                      kBloomBlurCount);

    // Begin swapchain render pass for composite + UI
    begin_swapchain_pass(fi, image_index);

    // Composite fullscreen triangle — blend scene (+bloom) onto swapchain
    bloom_.composite(cmd_buf_[fi], do_bloom);

    record_screen_layers(fi);

    vkCmdEndRenderPass(cmd_buf_[fi]);
    gpu_queries_.end(cmd_buf_[fi], fi); // the frame's, not a capture's readback
    const bool capturing = record_capture(cmd_buf_[fi], image_index);
    submit_and_present(fi, image_index, true, capturing, scene_capturing);
}

void Renderer::record_normal_pass(u32 fi, const std::array<f32, 16>& vp) {
    if (!frame_.terrain_normal_framebuffer() || !terrain_normal_pipeline_ || !terrain_tex_ds_ ||
        !shadow_ds_[fi] || terrain_mesh_.index_count() == 0)
        return;
    PROFILE_ZONE("Render::normals");
    std::array<VkClearValue, 2> cleared{};
    cleared[0].color = {{0.5f, 0.5f, 0.5f, 0.5f}}; // no terrain: zero normals
    cleared[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = frame_.scene_pass();
    begin.framebuffer = frame_.terrain_normal_framebuffer();
    begin.renderArea.extent = {window_width_, window_height_};
    begin.clearValueCount = static_cast<u32>(cleared.size());
    begin.pClearValues = cleared.data();
    vkCmdBeginRenderPass(cmd_buf_[fi], &begin, VK_SUBPASS_CONTENTS_INLINE);
    set_window_viewport(cmd_buf_[fi]);
    // The low fidelity terrain draws no normals (its DrawTerrainNormal is
    // empty, M212h): the pass only clears the target then, which keeps it
    // readable for what samples it (a fidelity 0 decal's light).
    if (fidelity() > 0) {
        vkc::bind_pipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS, terrain_normal_pipeline_);
        struct TerrainPC {
            f32 viewProj[16];
            f32 mapWidth;
            f32 mapHeight;
            f32 _pad0, _pad1;
            f32 eyeX, eyeY, eyeZ;
        } tpc{};
        static_assert(sizeof(TerrainPC) == 92, "matches terrain_vert's push block");
        std::memcpy(tpc.viewProj, vp.data(), sizeof(f32) * 16);
        tpc.mapWidth = terrain_map_width_;
        tpc.mapHeight = terrain_map_height_;
        camera_.eye_position(tpc.eyeX, tpc.eyeY, tpc.eyeZ);
        vkc::push_constants(cmd_buf_[fi], terrain_normal_layout_,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                            sizeof(tpc), &tpc);
        const std::array<VkDescriptorSet, 2> sets = {terrain_tex_ds_, shadow_ds_[fi]};
        vkc::bind_descriptor_sets(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  terrain_normal_layout_, 0, static_cast<u32>(sets.size()),
                                  sets.data(), 0, nullptr);
        VkBuffer vertices = terrain_mesh_.vertex_buffer();
        const VkDeviceSize no_offset = 0;
        vkCmdBindVertexBuffers(cmd_buf_[fi], 0, 1, &vertices, &no_offset);
        vkCmdBindIndexBuffer(cmd_buf_[fi], terrain_mesh_.index_buffer(), 0, VK_INDEX_TYPE_UINT32);
        vkc::draw_indexed(cmd_buf_[fi], terrain_mesh_.index_count(), 1, 0, 0, 0);

        // The normal decals (OverDrawDecals: TDecalsNormals, ...Alpha).
        record_decals(cmd_buf_[fi], fi, DecalTechnique::Normals, decal_normal_pipeline_, vp);
    }
    vkCmdEndRenderPass(cmd_buf_[fi]);
}

void Renderer::record_reflection_pass(u32 fi, const std::array<f32, 16>& vp) {
    // Moho's RenderReflections (M213b): the units, mirrored in the water's
    // plane, into a target of their own cleared to transparent black, which
    // the water's surface reads. The scene's pass and pipelines draw it; the
    // scene clears the depth it shares again. The pass's incoming dependency
    // (colour output) also waits for the last frame's water to have sampled
    // the target: a source stage takes in the stages before it, and a write
    // after a read needs no more than that.
    // Only the high fidelity water reads it (M213d).
    if (water_renderer_.has_water() && frame_.reflection_framebuffer() && fidelity() >= 2) {
        PROFILE_ZONE("Render::reflection");
        std::array<VkClearValue, 2> cleared{};
        cleared[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo mirror{};
        mirror.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        mirror.renderPass = frame_.scene_pass();
        mirror.framebuffer = frame_.reflection_framebuffer();
        mirror.renderArea.extent = {window_width_, window_height_};
        mirror.clearValueCount = static_cast<u32>(cleared.size());
        mirror.pClearValues = cleared.data();
        vkCmdBeginRenderPass(cmd_buf_[fi], &mirror, VK_SUBPASS_CONTENTS_INLINE);
        set_window_viewport(cmd_buf_[fi]);
        draw_meshes(cmd_buf_[fi], fi, mirrored_view_proj(vp, water_renderer_.water_elevation()),
                    MeshPass::Reflection);
        vkCmdEndRenderPass(cmd_buf_[fi]);
    }
}

// --- UI-only rendering (loading screen) ---

Renderer::VramUsage Renderer::vram_usage() const {
    VramUsage usage;
    if (!allocator_) return usage;
    const VkPhysicalDeviceMemoryProperties* props = nullptr;
    vmaGetMemoryProperties(allocator_, &props);
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(allocator_, budgets.data());
    for (u32 h = 0; props && h < props->memoryHeapCount; ++h) {
        if ((props->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) continue;
        usage.allocated_bytes += budgets[h].statistics.allocationBytes;
        usage.used_bytes += budgets[h].usage;
        usage.budget_bytes += budgets[h].budget;
    }
    return usage;
}

void Renderer::dump_frame(std::ostream& out) const {
    auto quad_line = [](const UIInstance& q) {
        return fmt::format("{:.3f} {:.3f} {:.3f} {:.3f} | {:.4f} {:.4f} {:.4f} {:.4f} | "
                           "{:.3f} {:.3f} {:.3f} {:.3f}",
                           q.rect[0], q.rect[1], q.rect[2], q.rect[3], q.uv[0], q.uv[1],
                           q.uv[2], q.uv[3], q.color[0], q.color[1], q.color[2], q.color[3]);
    };
    auto section = [&](const char* name, std::vector<std::string> lines) {
        std::sort(lines.begin(), lines.end());
        out << "[" << name << "] " << lines.size() << '\n';
        for (const auto& l : lines) out << l << '\n';
    };
    auto quads = [&](const std::vector<UIInstance>& qs) {
        std::vector<std::string> lines;
        lines.reserve(qs.size());
        for (const auto& q : qs) lines.push_back(quad_line(q));
        return lines;
    };
    auto ui_quads = [&](const std::vector<UIQuad>& qs) {
        std::vector<std::string> lines;
        lines.reserve(qs.size());
        for (const auto& q : qs) lines.push_back(quad_line(q.inst));
        return lines;
    };

    out << "[units]\n";
    unit_renderer_.dump(out);
    section("overlay", quads(overlay_renderer_.quads()));
    {
        // Each icon with its texture, in draw order: the order is Moho's
        // (ground, air, high-priority, selected; a badge over its icon), and
        // the entities' own, so as steady as any sorted section (M215c).
        const auto& qs = strategic_icon_renderer_.quads();
        const auto& textures = strategic_icon_renderer_.quad_textures();
        out << "[icons] " << qs.size() << '\n';
        for (size_t i = 0; i < qs.size(); ++i)
            out << (i < textures.size() ? textures[i] : std::string("?")) << " | "
                << quad_line(qs[i]) << '\n';
    }
    section("minimap-window", ui_quads(painted_minimap_));
    section("minimap-hud", ui_quads(minimap_renderer_.quads()));
    section("hud", quads(hud_renderer_.quads()));
    section("selection-info", quads(selection_info_renderer_.quads()));
    {
        std::vector<std::string> beams;
        for (const auto& b : beam_renderer_.drawn())
            beams.push_back(fmt::format(
                "{} {} | {:.3f} {:.3f} {:.3f} -> {:.3f} {:.3f} {:.3f} | {:.4f} | {:.3f} {:.3f} "
                "{:.3f} "
                "{:.3f} -> {:.3f} {:.3f} {:.3f} {:.3f} | {} | {:.4f} {:.4f} {:.4f}",
                b.effect_id, b.blueprint, b.start.x, b.start.y, b.start.z, b.end.x, b.end.y,
                b.end.z, b.thickness, b.start_color[0], b.start_color[1], b.start_color[2],
                b.start_color[3], b.end_color[0], b.end_color[1], b.end_color[2], b.end_color[3],
                b.blendmode, b.u_offset, b.v_start, b.v_end));
        section("beams", std::move(beams));
    }
    {
        std::vector<std::string> trails;
        for (const auto& t : trail_renderer_.drawn())
            trails.push_back(fmt::format(
                "{} {} | {:.3f} {:.3f} {:.3f} -> {:.3f} {:.3f} {:.3f} | t {:.4f} {:.4f} | u {:.4f} "
                "{:.4f} | {:.4f} | {} | {}",
                t.effect_id, t.blueprint, t.start.x, t.start.y, t.start.z, t.end.x, t.end.y,
                t.end.z, t.t_start, t.t_end, t.u_start, t.u_end, t.size, t.blendmode,
                t.under_water ? "under" : "over"));
        section("trails", std::move(trails));
    }
    std::vector<std::string> emitters;
    for (const auto& e : particle_system_.emitters()) {
        emitters.push_back(fmt::format("{} {} | {:.4f} {:.4f} {:.4f} | clock {:.0f} missed {} {}",
                                       e.effect_id, e.blueprint, e.position.x, e.position.y,
                                       e.position.z, e.clock, e.missed,
                                       e.seen ? "seen" : "unseen"));
    }
    section("emitters", std::move(emitters));
}

void Renderer::draw_meshes(VkCommandBuffer cmd, u32 fi, const std::array<f32, 16>& vp,
                           MeshPass stage) {
    // None when strategic zoom replaces 3D units with 2D icons.
    if (strategic_icon_renderer_.is_strategic_zoom() || unit_renderer_.mesh_groups().empty() ||
        !mesh_pipeline_)
        return;
    const bool mirrored = stage == MeshPass::Reflection;
    const f32 surface = water_renderer_.water_elevation();
    vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline_);

    // Push viewProj as first 64 bytes (bone offsets per-group below)
    struct MeshPushConstants {
        f32 viewProj[16];
        u32 boneBase;
        u32 bonesPerInst;
        f32 eyeX, eyeY, eyeZ;
        u32 technique; // MeshTechnique (M211b)
        u32 pass;      // a build technique's pass (M211f)
        f32 time;      // FA's time (M211f)
        u32 mirrored;  // drawn into the water's reflection (M213b)
        f32 surface;   // the water's elevation (M213b)
        u32 lane;      // mesh.fx's lane by graphics fidelity (M211m)
        u32 shadow_mode; // 0 none, 1 one tap, 2 five (M211m)
    } mesh_pc{};
    static_assert(sizeof(MeshPushConstants) == 112, "matches mesh_vert/frag's push block");
    std::memcpy(mesh_pc.viewProj, vp.data(), sizeof(f32) * 16);
    camera_.eye_position(mesh_pc.eyeX, mesh_pc.eyeY, mesh_pc.eyeZ);
    // (The reflection is seen from the eye mirrored in the water: mesh_frag
    // takes FA's view direction from viewProj, which is mirrored.)
    mesh_pc.mirrored = mirrored ? 1u : 0u;
    mesh_pc.surface = surface;
    mesh_pc.time = unit_renderer_.shader_time();

    // Bind fallback (1x1 white) as baseline — ensures set=0 is always valid
    VkDescriptorSet fallback_ds = texture_cache_.fallback_descriptor();
    if (fallback_ds) {
        vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 0, 1,
                                  &fallback_ds, 0, nullptr);
    }

    // Bind bone SSBO at set=1 (once for all groups)
    if (bone_ds_[fi]) {
        vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 1, 1,
                                  &bone_ds_[fi], 0, nullptr);
    }

    // Bind specteam fallback at set=2 (alpha=0 = no team color)
    VkDescriptorSet specteam_fallback = texture_cache_.specteam_fallback_descriptor();
    if (specteam_fallback) {
        vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 2, 1,
                                  &specteam_fallback, 0, nullptr);
    }

    // Bind normal map fallback at set=3 (flat normal = no perturbation)
    VkDescriptorSet normal_fallback = texture_cache_.normal_fallback_descriptor();
    if (normal_fallback) {
        vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 3, 1,
                                  &normal_fallback, 0, nullptr);
    }

    // Bind shadow descriptor set at set=4
    if (shadow_ds_[fi]) {
        vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 4, 1,
                                  &shadow_ds_[fi], 0, nullptr);
    }

    VkPipeline bound = mesh_pipeline_;
    for (auto& group : unit_renderer_.mesh_groups()) {
        if (!group.mesh || group.instance_count == 0) continue;
        // Moho's buckets (M213b): a technique's render stage puts it
        // before the water or after it, or after the effects too (the
        // shields', M211k); only units are reflected.
        const MeshTechnique technique = drawn_technique(group);
        const bool after_effects = is_post_effect_technique(technique);
        const bool after_water = is_post_water_technique(technique);
        if ((stage == MeshPass::AfterEffects) != after_effects ||
            (stage == MeshPass::BeforeWater && after_water) ||
            (stage == MeshPass::AfterWater && !after_water) ||
            (stage == MeshPass::Reflection && !group.reflected))
            continue;

        // Bind per-group albedo texture descriptor (always bind to avoid
        // stale set=0 from prior group)
        VkDescriptorSet albedo_ds = group.texture_ds ? group.texture_ds : fallback_ds;
        if (albedo_ds) {
            vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 0, 1,
                                      &albedo_ds, 0, nullptr);
        }

        // Bind per-group specteam texture descriptor (always bind to avoid
        // stale set=2 from prior group)
        VkDescriptorSet spec_ds = group.specteam_ds ? group.specteam_ds : specteam_fallback;
        if (spec_ds) {
            vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 2, 1,
                                      &spec_ds, 0, nullptr);
        }

        // Bind per-group normal map descriptor (always bind to avoid
        // stale set=3 from prior group)
        VkDescriptorSet norm_ds = group.normal_ds ? group.normal_ds : normal_fallback;
        if (norm_ds) {
            vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 3, 1,
                                      &norm_ds, 0, nullptr);
        }

        // The mesh's lookup texture (set=5) and secondary (set=6),
        // transparent black without them
        VkDescriptorSet lookup_ds = group.lookup_ds ? group.lookup_ds : specteam_fallback;
        if (lookup_ds) {
            vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 5, 1,
                                      &lookup_ds, 0, nullptr);
        }
        VkDescriptorSet secondary_ds = group.secondary_ds ? group.secondary_ds : specteam_fallback;
        if (secondary_ds) {
            vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 6, 1,
                                      &secondary_ds, 0, nullptr);
        }

        VkBuffer vbufs[] = {group.mesh->vertex_buf.buffer, unit_renderer_.mesh_instance_buffer()};
        VkDeviceSize buf_offsets[] = {0, static_cast<VkDeviceSize>(group.instance_offset) *
                                             sizeof(MeshInstance)};
        vkCmdBindVertexBuffers(cmd, 0, 2, vbufs, buf_offsets);
        vkCmdBindIndexBuffer(cmd, group.mesh->index_buf.buffer, 0, VK_INDEX_TYPE_UINT32);

        // The technique's passes, one after the other, as FA draws a
        // batch's (M211f). Fading and build groups come last, with the
        // pipelines that blend them (the layouts are compatible: the
        // sets bound stay bound). A build technique's base pass blends
        // colour only (Aeon's at alpha 1: opaque); UEF's and Cybran's
        // overlays blend alpha too, Aeon's colour only.
        std::array<VkPipeline, 2> passes = {group.fading ? mesh_fade_pipeline_ : mesh_pipeline_,
                                            VK_NULL_HANDLE};
        if (technique == MeshTechnique::UEFBuild || technique == MeshTechnique::CybranBuild)
            passes[1] = mesh_overlay_pipeline_;
        // (At Low, Aeon's build is one blended pass, M211n.)
        else if (technique == MeshTechnique::AeonBuild && fidelity() > 0)
            passes[1] = mesh_fade_pipeline_;
        // The build effects' (M211g): AlphaFade blends colour and alpha,
        // UEF's cube colour only and writes no depth.
        else if (technique == MeshTechnique::AlphaFade) passes[0] = mesh_overlay_pipeline_;
        else if (technique == MeshTechnique::UEFBuildCube) passes[0] = mesh_cube_pipeline_;
        else if (is_feedback_technique(technique)) passes[0] = mesh_feedback_pipeline_;
        // The shields' (M211k), by mesh.fx's states; Cybran's draws twice,
        // the second time pushed out along its normal.
        else if (is_shield_technique(technique)) {
            const ShieldPasses shield = shield_passes(technique);
            passes[0] = shield_pipelines_[static_cast<u32>(shield.state)];
            // (At Low, Cybran's is one pass, M211n.)
            if (shield.count > 1 && fidelity() > 0) passes[1] = passes[0];
        }
        // A personal shield's (M211l): the unit as its base technique, then
        // the electric shell, blended, its depth written (mesh.fx's P1
        // leaves the depth state at D3D's default).
        else if (is_personal_shield_technique(technique))
            passes[1] = shield_pipelines_[static_cast<u32>(ShieldState::BlendDepthWrite)];
        else if (technique == MeshTechnique::UnitPlace) {
            passes[0] = mesh_overlay_pipeline_;
        }
        mesh_pc.boneBase = group.bone_base_offset;
        mesh_pc.bonesPerInst = group.bones_per_instance;
        for (u32 pass = 0; pass < passes.size() && passes[pass]; ++pass) {
            if (passes[pass] != bound) {
                vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, passes[pass]);
                bound = passes[pass];
            }
            // The first pass draws as the base technique (a personal
            // shield's unit), the rest as the technique's own
            MeshTechnique drawn = pass == 0 ? base_technique(technique) : technique;
            // The lane mesh.fx picks by graphics fidelity (M211m). A Seraphim
            // personal shield has no Medium lane: below High it draws its Low
            // one, the unit as LowFiUnitFalloffPS and the shell as
            // PhaseShieldPS.
            int lane = fidelity();
            if (technique == MeshTechnique::SeraphimPersonalShield && lane < 2) {
                lane = 0;
                if (pass == 1) drawn = MeshTechnique::PhaseShield;
            }
            mesh_pc.technique = static_cast<u32>(drawn);
            mesh_pc.pass = pass;
            mesh_pc.lane = static_cast<u32>(lane);
            // Meshes take shadows above shadow fidelity 1, and not on the
            // Low lane: one tap, or High's five at 3 with ren_ShadowBlur.
            // With no light camera this frame, none (M210c).
            const int shadows = shadow_fidelity();
            mesh_pc.shadow_mode =
                lane == 0 || shadows <= 1 || !shadow_camera_valid_
                    ? 0u
                    : (lane == 2 && shadows == 3 && video_options_.shadow_blur ? 2u : 1u);
            vkc::push_constants(cmd, mesh_layout_,
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                sizeof(mesh_pc), &mesh_pc);
            vkc::draw_indexed(cmd, group.mesh->index_count, group.instance_count, 0, 0, 0);
            if (stage != MeshPass::Reflection && mesh_pc.technique < mesh_draws_.size())
                ++mesh_draws_[mesh_pc.technique];
        }
    }
}

void Renderer::copy_and_continue(VkCommandBuffer cmd, VkRenderPass next) {
    vkCmdEndRenderPass(cmd);
    copy_refraction(cmd);
    VkRenderPassBeginInfo again{};
    again.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    again.renderPass = next;
    again.framebuffer = frame_.scene_framebuffer();
    again.renderArea.extent = {window_width_, window_height_};
    vkCmdBeginRenderPass(cmd, &again, VK_SUBPASS_CONTENTS_INLINE);
}

void Renderer::copy_refraction(VkCommandBuffer cmd) {
    // The frame, drawn, to be read; the copy's previous contents (read by
    // the last frame's water) discarded.
    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = frame_.scene_color().image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);
    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = frame_.refraction().image;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_dst);

    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {window_width_, window_height_, 1};
    vkCmdCopyImage(cmd, frame_.scene_color().image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   frame_.refraction().image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // The copy for the water to read; the frame back to be drawn on.
    std::array<VkImageMemoryBarrier, 2> after{};
    after[0] = to_dst;
    after[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    after[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    after[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    after[1] = to_dst;
    after[1].image = frame_.scene_color().image;
    after[1].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    after[1].dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    after[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    after[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // And the first pass's depth, which the second goes on testing.
    VkMemoryBarrier depth{};
    depth.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    depth.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depth.dstAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(
        cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        0, 1, &depth, 0, nullptr, static_cast<u32>(after.size()), after.data());
}

void Renderer::render_ui_only(lua_State* L, ui::UIControlRegistry* ui_registry) {
    // (debug removed)
    minimap_renderer_.begin_frame(); // no world, so no minimap this frame
    u32 fi = frame_index_ % FRAMES_IN_FLIGHT;
    u32 image_index = 0;
    if (!begin_frame_slot(fi, image_index)) return;
    vkResetFences(device_, 1, &render_fence_[fi]);

    // Set frame index on UI renderer for correct double-buffering
    ui_renderer_.set_frame_index(fi);

    // Flush pending texture uploads BEFORE starting the command buffer
    // (flush_uploads uses its own one-shot command buffers with vkQueueWaitIdle)
    texture_cache_.flush_uploads(4);

    // The frame's step, as render() takes it (fixed for scripted runs and
    // captures).
    {
        const f64 now = glfwGetTime();
        f32 dt = (last_frame_time_ > 0.0) ? static_cast<f32>(now - last_frame_time_) : 0.0f;
        last_frame_time_ = now;
        if (fixed_frame_dt_ > 0.0f) dt = fixed_frame_dt_;
        frame_dt_ = dt;
    }

    // The UI, before the command buffer: its frame callbacks (the movies
    // play on there), its events, then its quads, with the movies' new
    // frames staged for the copy below.
    if (ui_registry && L) {
        if (frame_dt_ > 0.0f && frame_dt_ < 1.0f)
            ui_dispatch_.update_controls(L, *ui_registry, static_cast<f64>(frame_dt_));
        ui_dispatch_.dispatch_events(L, *ui_registry);
        movie_textures_.prepare(*ui_registry, fi);
        update_system_cursor(L);
        ui_renderer_.update(L, *ui_registry, texture_cache_, font_cache_, window_width_,
                            window_height_, static_cast<f32>(ui_dispatch_.mouse_x()),
                            static_cast<f32>(ui_dispatch_.mouse_y()));
    }

    // Begin command buffer
    begin_commands(fi);

    // The movies' new frames (before the render pass).
    movie_textures_.record(cmd_buf_[fi]);

    // Begin swapchain render pass (NOT frame_.scene_pass())
    begin_swapchain_pass(fi, image_index);

    if (ui_registry && L && ui_pipeline_ && ui_renderer_.quad_count() > 0) {
        vkc::bind_pipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline_);
        ui_renderer_.render(cmd_buf_[fi], ui_layout_, window_width_, window_height_);
    }

    vkCmdEndRenderPass(cmd_buf_[fi]);
    const bool capturing = record_capture(cmd_buf_[fi], image_index);
    submit_and_present(fi, image_index, false, capturing, false);
}

void Renderer::update_frame_scene(u32 fi, const std::array<f32, 16>& vp, const Frustum& frustum,
                                  const sim::FrameView& view, sim::WorldEvents& events,
                                  const BuildGhost* ghost, lua_State* L,
                                  ui::UIControlRegistry* ui_registry,
                                  const std::unordered_set<u32>* selected_ids) {
    // What the player's army sees this tick (everything, with the fog off)
    recon_.set_focus_army(fog_enabled_ ? player_army_ : -1);
    recon_.update(view, events.intel_flushes);
    events.intel_flushes.clear();
    // A playable rect the scripts synced since: what's outside it now hides
    playable_rect_.apply(view);

    // Before the meshes: its planned sites are ghosts among them
    command_graph_renderer_.set_highlight(highlight_command_, hovered_);
    command_graph_renderer_.update(view, camera_, selected_ids, player_army_, texture_cache_, L,
                                   unit_renderer_.shader_time() / 10.0f, window_height_,
                                   (!ui_keys_blocked_ && (is_key_pressed(GLFW_KEY_LEFT_SHIFT) ||
                                                          is_key_pressed(GLFW_KEY_RIGHT_SHIFT))) ||
                                       command_drag_held_,
                                   fi);

    const std::vector<WorldMeshDraw> world_meshes =
        ui_registry ? shown_world_meshes(*ui_registry) : std::vector<WorldMeshDraw>{};
    // Update unit instances (mesh + cube fallback + texture resolution + frustum culling)
    {
        PROFILE_ZONE("Render::unit_update");
        // Strategic zoom draws icons, not meshes (as StrategicIconRenderer
        // decides it below, from the same camera)
        const bool meshes_drawn = camera_.eye_distance() < StrategicIconRenderer::ZOOM_THRESHOLD;
        unit_renderer_.set_ghost_slots(
            1 + static_cast<u32>(ghost ? ghost->line.size() : 0) +
            static_cast<u32>(command_graph_renderer_.planned_sites().size()) +
            static_cast<u32>(formation_ghosts_.size()) +
            static_cast<u32>(feedback_blips_.blips().size()) +
            static_cast<u32>(world_meshes.size()));
        unit_renderer_.update(view, mesh_cache_, L, &texture_cache_, &camera_, selected_ids,
                              &frustum, meshes_drawn);
    }
    // A bone SSBO the update grew is a new buffer for this slot's set.
    if (bone_ds_[fi] && bone_ds_generation_[fi] != unit_renderer_.bone_ssbo_generation(fi))
        write_bone_descriptor(fi);

    // Build preview ghost — a semi-transparent mesh where input places it
    if (ghost && !ghost->blueprint_id.empty()) {
        // Green = valid, Red = invalid
        f32 gr = ghost->valid ? 0.2f : 1.0f;
        f32 gg = ghost->valid ? 0.9f : 0.2f;
        f32 gb = ghost->valid ? 0.3f : 0.2f;

        const GPUMesh* ghost_mesh = mesh_cache_.get(ghost->blueprint_id, L);
        if (ghost_mesh) {
            unit_renderer_.inject_ghost(ghost_mesh, ghost->x, ghost->y, ghost->z, gr, gg, gb,
                                        &texture_cache_);
        }
        // A drag's other sites, or a build template's other structures, each
        // its own blueprint's mesh
        for (const BuildGhost& site : ghost->line) {
            const GPUMesh* mesh = site.blueprint_id == ghost->blueprint_id
                                      ? ghost_mesh
                                      : mesh_cache_.get(site.blueprint_id, L);
            if (!mesh) continue;
            unit_renderer_.inject_ghost(mesh, site.x, site.y, site.z, site.valid ? 0.2f : 1.0f,
                                        site.valid ? 0.9f : 0.2f, site.valid ? 0.3f : 0.2f,
                                        &texture_cache_);
        }
    }
    for (const auto& site : command_graph_renderer_.planned_sites()) {
        if (const GPUMesh* mesh = mesh_cache_.get(site.blueprint, L)) {
            unit_renderer_.inject_ghost(mesh, site.position.x, site.position.y, site.position.z,
                                        0.2f, 0.9f, 0.3f, &texture_cache_);
        }
    }
    // Moho's formation preview tint, 0xD8D8D800
    constexpr f32 kFormationTint = 0xD8 / 255.0f;
    for (const FormationGhost& ghost : formation_ghosts_) {
        if (const GPUMesh* mesh = mesh_cache_.get(ghost.blueprint_id, L)) {
            unit_renderer_.inject_ghost(mesh, ghost.position.x, ghost.position.y, ghost.position.z,
                                        kFormationTint, kFormationTint, 0.0f, &texture_cache_,
                                        ghost.heading);
        }
    }

    // The frame's step, which the particles and overlays run on, with or
    // without a UI (an offscreen test has none).
    {
        const f64 now = glfwGetTime();
        f32 dt = (last_frame_time_ > 0.0) ? static_cast<f32>(now - last_frame_time_) : 0.0f;
        last_frame_time_ = now;
        if (fixed_frame_dt_ > 0.0f) dt = fixed_frame_dt_;
        frame_dt_ = dt;
        wave_clock_ += static_cast<f64>(dt);
    }
    // The order marks, aged by the frame (Moho's UpdateCommandFeedbackBlips)
    feedback_blips_.update(frame_dt_);
    inject_feedback_blips(L, world_meshes);

    // Update UI quads (walk control tree, read LazyVar positions)
    if (ui_registry) {
        PROFILE_ZONE("Render::ui_update");
        const f32 dt = frame_dt_;
        total_time_ += dt;
        if (dt > 0.0f && dt < 1.0f) {
            ui_renderer_.advance_animations(L, *ui_registry, dt);
            ui_dispatch_.update_controls(L, *ui_registry, static_cast<f64>(dt));
        }
        ui_dispatch_.dispatch_events(L, *ui_registry);
        // FA's minimap WorldView shows the minimap, drawn with the UI.
        WorldViewPainter minimap_painter;
        if (!legacy_hud_active_) {
            painted_minimap_.clear();
            minimap_painter = [&](const ui::WorldView& wv, const ui::ControlRect& r,
                                  std::vector<UIQuad>& out) {
                const size_t first = out.size();
                std::optional<PlayableRect> resources;
                if (wv.resource_rendering() && terrain_) {
                    resources = playable_rect_.rect().value_or(
                        PlayableRect{0, 0, static_cast<i32>(terrain_->map_width()),
                                     static_cast<i32>(terrain_->map_height())});
                }
                minimap_renderer_.paint(view, camera_, texture_cache_, r.x, r.y, r.w, r.h,
                                        window_width_, window_height_, selected_ids, L, out,
                                        resources);
                painted_minimap_.insert(painted_minimap_.end(),
                                        out.begin() + static_cast<std::ptrdiff_t>(first), out.end());
            };
        }
        movie_textures_.prepare(*ui_registry, fi);
        update_system_cursor(L);
        ui_renderer_.update(L, *ui_registry, texture_cache_, font_cache_,
                            window_width_, window_height_,
                            static_cast<f32>(ui_dispatch_.mouse_x()),
                            static_cast<f32>(ui_dispatch_.mouse_y()),
                            minimap_painter);
    }

    // Stage fog of war data from visibility grid (CPU side)
    if (fog_enabled_ && fog_renderer_.initialized() && view.cur()) {
        if (player_army_ >= 0 && view.cur()->sight.army == player_army_)
            fog_renderer_.stage(view.cur()->sight);
        else fog_renderer_.stage_clear(); // an observer's: all visible
    }

    // Effects are made at the player's graphics fidelity (graphics_Fidelity).
    beam_renderer_.set_fidelity(fidelity());
    trail_renderer_.set_fidelity(fidelity());
    particle_system_.set_fidelity(fidelity());
    // FA's beams, before the overlay, which leaves the ones drawn to them (M214a)
    beam_renderer_.update(view, camera_, beam_bp_cache_, texture_cache_, L, &recon_,
                          unit_renderer_.shader_time(), fi);
    // And its trails, which likewise leave their dots to them (M214b)
    trail_renderer_.update(view, camera_, &frustum, trail_bp_cache_, texture_cache_, L, &recon_,
                           fi);
    // And its particles, likewise; a new tick's emission, then this frame's quads (M214c)
    {
        PROFILE_ZONE("Render::particle_update");
        // The waves in view emit on the system clock (M213c)
        if (view.cur()) {
            waves_emitted_.clear();
            wave_system_.update(frustum, frame_dt_, view.cur()->tick, wave_clock_, waves_emitted_);
            for (const WaveParticle& w : waves_emitted_) particle_system_.add_wave(w);
        }
        particle_system_.update(view, camera_, &frustum, emitter_bp_cache_, L, terrain_);
        particle_renderer_.update(particle_system_, texture_cache_, fi);
    }

    selection_renderer_.update(view, camera_, window_height_, selected_ids, hovered_, player_army_,
                               drag_box_, texture_cache_, mesh_cache_, L, fi);

    // FA's range overlays (Moho's RangeRenderer), for the focus army
    {
        PROFILE_ZONE("Render::range_update");
        const RangeScene scene = collect_range_scene(
            *range_overlays_, view, frustum, player_army_, selected_ids, hovered_,
            ghost ? &ghost->blueprint_id : nullptr, ghost ? ghost->cursor_x : 0.0f,
            ghost ? ghost->cursor_z : 0.0f, range_blueprints_, L);
        const auto& rect = camera_.playable_rect();
        const f32 span = std::max(rect[2] - rect[0], rect[3] - rect[1]);
        range_renderer_.update(range_batches(*range_overlays_, scene), range_overlays_->settings(),
                               span, camera_.zoom() / camera_.max_zoom(), fi);
    }

    // Update game overlays (health bars, selection circles, game over)
    {
        PROFILE_ZONE("Render::overlay_update");
        const i32 game_result = legacy_hud_active_ && view.cur() ? view.cur()->player_result : 0;
        overlay_renderer_.set_hovered(hovered_);
        overlay_renderer_.update(view, events, camera_, vp, selected_ids, texture_cache_,
                                 window_width_, window_height_, game_result, frame_dt_, &frustum,
                                 ghost);
    }

    // FA's water: this frame's camera and time (M213a)
    water_renderer_.update(camera_, vp, unit_renderer_.shader_time(), fi);
    // The sky: its time is the tick and the interpolant, unwrapped (M210b)
    sky_renderer_.update(camera_, vp, view.cur() ? view.cur()->tick : 0, view.alpha(), fi);

    // Scripts' decals and splats: this tick's, as the player's army sees
    // them (M212c)
    if (terrain_) {
        PROFILE_ZONE("Render::runtime_decals");
        f32 ex = 0;
        f32 ey = 0;
        f32 ez = 0;
        camera_.eye_position(ex, ey, ez);
        const f32 aspect = static_cast<f32>(window_width_) / static_cast<f32>(window_height_);
        runtime_decals_.update(view.cur(), fog_enabled_ ? player_army_ : -1, *terrain_,
                               terrain_mesh_, camera_.view(), {ex, ey, ez},
                               camera_.tan_half_fov_y(aspect) * aspect, frustum, texture_cache_, fi,
                               fidelity(), ui_registry ? &ui_registry->user_decals() : nullptr);
    }

    // The terrain's Time (M212f): set when the terrain would re-tessellate,
    // so TTerrainGlow's lava stands still under a still camera, as in FA.
    if (const sim::WorldSnapshot* cur = view.cur()) {
        const u64 decals = static_cast<u64>(runtime_decals_.decal_draws().size()) |
                           (static_cast<u64>(runtime_decals_.splat_count()) << 32);
        terrain_time_.update(camera_.view(), decals, static_cast<f32>(cur->tick) + view.alpha());
    }

    // Update minimap (terrain bg, units' icons, camera frustum box)
    if (legacy_hud_active_) {
        minimap_renderer_.update(view, camera_, texture_cache_, selected_ids, window_width_,
                                 window_height_, L);
    }

    // Update strategic icons (zoom-dependent 2D icons replacing 3D meshes)
    strategic_icon_renderer_.update(view, camera_, vp, selected_ids, texture_cache_, window_width_,
                                    window_height_, L);

    {
        bool resources = !camera_.free();
        if (L) {
            lua_pushstring(L, "__osc_world_view");
            lua_rawget(L, LUA_REGISTRYINDEX);
            if (const auto* wv = static_cast<const ui::WorldView*>(lua_touserdata(L, -1))) {
                resources = resources && wv->resource_rendering();
            }
            lua_pop(L, 1);
        }
        std::vector<ResourceIcon> icons;
        if (resources && terrain_ && view.cur()) {
            const map::Terrain& terrain = *terrain_;
            const PlayableRect whole{0, 0, static_cast<i32>(terrain.map_width()),
                                     static_cast<i32>(terrain.map_height())};
            icons = renderer::resource_icons(
                view.cur()->deposits, camera_, static_cast<f32>(window_width_),
                static_cast<f32>(window_height_), playable_rect_.rect().value_or(whole),
                [&](f32 x, f32 z) { return terrain.get_terrain_height(x, z); });
        }
        resource_icon_renderer_.update(icons, texture_cache_, fi);
        resource_icon_time_ = static_cast<f32>(glfwGetTime());
    }

    if (legacy_hud_active_) {
        // Update economy HUD
        hud_renderer_.update(view, player_army_, font_cache_, texture_cache_,
                              window_width_, window_height_);

        // Update selection info panel
        selection_info_renderer_.update(view, selected_ids, font_cache_, texture_cache_,
                                        strategic_icon_renderer_.atlas_descriptor(),
                                        window_width_, window_height_);
    }

    // Update profile overlay
    profile_overlay_.update(font_cache_, texture_cache_,
                             window_width_, window_height_);
}

void Renderer::begin_commands(u32 fi) {
    vkResetCommandBuffer(cmd_buf_[fi], 0);
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_buf_[fi], &begin_info);
}

void Renderer::set_window_viewport(VkCommandBuffer cmd) const {
    VkViewport viewport{};
    viewport.width = static_cast<f32>(window_width_);
    viewport.height = static_cast<f32>(window_height_);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {window_width_, window_height_};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void Renderer::begin_swapchain_pass(u32 fi, u32 image_index) {
    // render_pass_ has 2 attachments: color + depth
    std::array<VkClearValue, 2> clear{};
    clear[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clear[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = render_pass_;
    rp.framebuffer = framebuffers_[image_index];
    rp.renderArea.extent = {window_width_, window_height_};
    rp.clearValueCount = static_cast<u32>(clear.size());
    rp.pClearValues = clear.data();
    vkCmdBeginRenderPass(cmd_buf_[fi], &rp, VK_SUBPASS_CONTENTS_INLINE);
    set_window_viewport(cmd_buf_[fi]);
}

void Renderer::record_main_pass(u32 fi, const std::array<f32, 16>& vp) {
    // Always render scene to offscreen HDR image (frame_.scene_pass()).
    // Composite pass copies scene to swapchain, adding bloom when enabled.
    std::array<VkClearValue, 2> clear_values{};
    // Black, and no glow, as Moho clears the head; the sky dome draws over it
    clear_values[0].color = {{clear_color_[0], clear_color_[1], clear_color_[2], clear_color_[3]}};
    clear_values[1].depthStencil = {1.0f, 0};

    // Split around the water on a map with it (M213a), and before the
    // refracting particles on a frame with them (M214d), each for a copy of
    // the frame. Below graphics fidelity 2 there is neither: the low
    // fidelity water refracts nothing, and Moho draws no refracting effects
    // (M213d).
    const bool high_water = water_renderer_.has_water() && fidelity() >= 2;
    const bool refracting = fidelity() >= 2 && particle_renderer_.refracting();
    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = high_water || refracting ? frame_.first_pass() : frame_.scene_pass();
    rp_begin.framebuffer = frame_.scene_framebuffer();
    rp_begin.renderArea.extent = {window_width_, window_height_};
    rp_begin.clearValueCount = static_cast<u32>(clear_values.size());
    rp_begin.pClearValues = clear_values.data();
    vkCmdBeginRenderPass(cmd_buf_[fi], &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    // Dynamic viewport + scissor
    set_window_viewport(cmd_buf_[fi]);

    // 0. The sky dome, before the terrain (WRenViewport::RenderSkyDome; M210b),
    // unless ren_SkyDome is off (the render_skydome option): the clear shows
    if (video_options_.skydome) sky_renderer_.record(cmd_buf_[fi], fi);

    // 1. Draw terrain: at graphics fidelity 0 the low fidelity terrain
    // (LowFidelityTerrain, M212h), else the map's own shader (Medium and
    // High alike)
    const bool low_terrain = fidelity() == 0 && terrain_low_pipeline_;
    if (terrain_mesh_.index_count() > 0 && terrain_pipeline_) {
        vkc::bind_pipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                           low_terrain ? terrain_low_pipeline_ : terrain_pipeline_);

        // Push constants: viewProj(64) + mapW(4) + mapH(4) + Time and a
        // pad(8) + eye(12) = 92B
        struct TerrainPC {
            f32 viewProj[16];
            f32 mapWidth;
            f32 mapHeight;
            f32 terrainTime, _pad1;
            f32 eyeX, eyeY, eyeZ;
        } tpc{};
        static_assert(sizeof(TerrainPC) == 92, "matches terrain_vert/frag's push block");
        std::memcpy(tpc.viewProj, vp.data(), sizeof(f32) * 16);
        tpc.mapWidth = terrain_map_width_;
        tpc.mapHeight = terrain_map_height_;
        tpc.terrainTime = terrain_time_.value();
        camera_.eye_position(tpc.eyeX, tpc.eyeY, tpc.eyeZ);

        vkc::push_constants(cmd_buf_[fi], terrain_layout_,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                            sizeof(tpc), &tpc);

        // Bind terrain texture descriptor set (set=0)
        if (terrain_tex_ds_) {
            vkc::bind_descriptor_sets(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      terrain_layout_, 0, 1, &terrain_tex_ds_, 0, nullptr);
        }
        // Bind shadow descriptor set (set=1)
        if (shadow_ds_[fi]) {
            vkc::bind_descriptor_sets(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      terrain_layout_, 1, 1, &shadow_ds_[fi], 0, nullptr);
        }

        VkBuffer vbufs[] = {terrain_mesh_.vertex_buffer()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd_buf_[fi], 0, 1, vbufs, offsets);
        vkCmdBindIndexBuffer(cmd_buf_[fi], terrain_mesh_.index_buffer(), 0,
                             VK_INDEX_TYPE_UINT32);
        vkc::draw_indexed(cmd_buf_[fi], terrain_mesh_.index_count(), 1, 0, 0, 0);
    }

    // 2. The decals, as HighFidelityTerrain's DrawNormals draws them: the
    // glow masks, the Albedo and the AlbedoXP passes (DrawDecalPass), the
    // splats (DrawSplatComposite), then the glowing decals
    // (DrawGlowingDecals). Each pass the map's decals (M212b) and then the
    // scripts' (M212c), faded by their LOD (GetLODAlpha). The normal decals
    // drew in the normal pass (M212e).
    // Medium and Low have no AlbedoXP pass; Low draws the water's albedo
    // decals here, before the water (LowFidelityTerrain::DrawNormals, M212h).
    record_decals(cmd_buf_[fi], fi, DecalTechnique::GlowMask, decal_glow_mask_pipeline_, vp);
    record_decals(cmd_buf_[fi], fi, DecalTechnique::Albedo, decal_pipeline_, vp);
    if (fidelity() >= 2)
        record_decals(cmd_buf_[fi], fi, DecalTechnique::AlbedoXP, decal_pipeline_, vp);
    if (fidelity() == 0)
        record_decals(cmd_buf_[fi], fi, DecalTechnique::WaterAlbedo, decal_water_pipeline_, vp);
    if (decals_enabled_ && terrain_ && terrain_tex_ds_ && shadow_ds_[fi]) {
        f32 ex = 0;
        f32 ey = 0;
        f32 ez = 0;
        camera_.eye_position(ex, ey, ez);
        runtime_decals_.draw_splats(cmd_buf_[fi], fi, vp, {ex, ey, ez},
                                    static_cast<f32>(terrain_->map_width()),
                                    static_cast<f32>(terrain_->map_height()), terrain_tex_ds_,
                                    shadow_ds_[fi], fidelity() == 0);
    }
    record_decals(cmd_buf_[fi], fi, DecalTechnique::Glow, decal_glow_pipeline_, vp);

    // WRenViewport::RenderCompositeTerrain: DrawTerrainSkirt after DrawNormals
    if (video_options_.skirt && terrain_skirt_pipeline_ && terrain_mesh_.skirt_index_count() > 0) {
        vkc::bind_pipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS, terrain_skirt_pipeline_);
        vkc::push_constants(cmd_buf_[fi], terrain_skirt_layout_,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                            sizeof(f32) * 16, vp.data());
        VkBuffer vbufs[] = {terrain_mesh_.vertex_buffer()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd_buf_[fi], 0, 1, vbufs, offsets);
        vkCmdBindIndexBuffer(cmd_buf_[fi], terrain_mesh_.index_buffer(), 0, VK_INDEX_TYPE_UINT32);
        vkc::draw_indexed(cmd_buf_[fi], terrain_mesh_.skirt_index_count(), 1,
                          terrain_mesh_.skirt_first_index(), 0, 0);
    }

    // 2b. The range overlays, on the terrain before the meshes
    // (WRenViewport::Render's RangeRenderer::Render)
    range_renderer_.render(cmd_buf_[fi], window_width_, window_height_, vp.data(), fi);

    // 3. The meshes (real SCM models with GPU skinning): on a map with water,
    // those Moho draws before it (M213b)
    draw_meshes(cmd_buf_[fi], fi, vp,
                water_renderer_.has_water() ? MeshPass::BeforeWater : MeshPass::All);

    // 4. Draw cube fallback units (skip when strategic zoom active)
    if (!strategic_icon_renderer_.is_strategic_zoom() &&
        unit_renderer_.cube_instance_count() > 0 && unit_pipeline_) {
        vkc::bind_pipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS, unit_pipeline_);

        // Bind shadow descriptor set at set=0
        if (shadow_ds_[fi]) {
            vkc::bind_descriptor_sets(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS, unit_layout_,
                                      0, 1, &shadow_ds_[fi], 0, nullptr);
        }

        struct UnitPC {
            f32 viewProj[16];
            f32 eyeX, eyeY, eyeZ;
        } upc{};
        std::memcpy(upc.viewProj, vp.data(), sizeof(f32) * 16);
        camera_.eye_position(upc.eyeX, upc.eyeY, upc.eyeZ);
        vkc::push_constants(cmd_buf_[fi], unit_layout_,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                            sizeof(upc), &upc);

        VkBuffer vbufs[] = {unit_renderer_.cube_vertex_buffer(),
                            unit_renderer_.cube_instance_buffer()};
        VkDeviceSize offsets[] = {0, 0};
        vkCmdBindVertexBuffers(cmd_buf_[fi], 0, 2, vbufs, offsets);
        vkCmdBindIndexBuffer(cmd_buf_[fi], unit_renderer_.cube_index_buffer(), 0,
                             VK_INDEX_TYPE_UINT32);
        vkc::draw_indexed(cmd_buf_[fi], unit_renderer_.cube_index_count(),
                          unit_renderer_.cube_instance_count(), 0, 0, 0);
    }

    // 4b. FA's particles and trails under the water, a negative SortOrder's
    // (M214b-c)
    particle_renderer_.render(cmd_buf_[fi], window_width_, window_height_, vp.data(), true, fi);
    trail_renderer_.render(cmd_buf_[fi], window_width_, window_height_, vp.data(), true, fi);

    // 5. FA's water (M213a), as CWorldView draws it: its alpha mask (alpha 0
    // over open water), a copy of the frame so far, then the surface, which
    // refracts the copy, in a pass that goes on from the first (and, with
    // refracting particles to come, ends as it does).
    // Below graphics fidelity 2, Water_LowFidelity, with no mask and no copy
    // (LowFidelityWater, M213d).
    if (water_renderer_.has_water()) {
        if (high_water) {
            water_renderer_.render_mask(cmd_buf_[fi], window_width_, window_height_, fi);
            copy_and_continue(cmd_buf_[fi],
                              refracting ? frame_.middle_pass() : frame_.second_pass());
            water_renderer_.render_surface(cmd_buf_[fi], window_width_, window_height_, fi);
        } else {
            water_renderer_.render_surface_low(cmd_buf_[fi], window_width_, window_height_, fi);
        }
        // The water's albedo decals on its surface (M212g), as the terrain
        // draws them once the water is down (Medium and High; Low drew them).
        if (fidelity() > 0)
            record_decals(cmd_buf_[fi], fi, DecalTechnique::WaterAlbedo, decal_water_pipeline_, vp);
        // The meshes Moho draws after the water (M213b), which writes no
        // depth: over it, unrefracted.
        draw_meshes(cmd_buf_[fi], fi, vp, MeshPass::AfterWater);
    }

    // 5b. FA's beams (M214a), then particles (M214c) and trails (M214b), as
    // Moho's CWorldParticles::RenderEffects draws them
    beam_renderer_.render(cmd_buf_[fi], window_width_, window_height_, vp.data(), fi);
    particle_renderer_.render(cmd_buf_[fi], window_width_, window_height_, vp.data(), false, fi);
    trail_renderer_.render(cmd_buf_[fi], window_width_, window_height_, vp.data(), false, fi);
    // The last of the meshes, after the effects above the water: the
    // shields, their fills and their impacts (M211k; RenderMeshes(0x28)).
    draw_meshes(cmd_buf_[fi], fi, vp, MeshPass::AfterEffects);
    // The order lines and waypoints, over the world (TCommand)
    command_graph_renderer_.render(cmd_buf_[fi], window_width_, window_height_, vp.data(), fi);
    selection_renderer_.render(cmd_buf_[fi], window_width_, window_height_, vp.data(), fi);

    // 5c. FA's refracting particles (M214d), as WRenViewport's
    // RenderRefractingEffects draws them: last, over a copy of the finished
    // frame.
    if (refracting) {
        copy_and_continue(cmd_buf_[fi], frame_.second_pass());
        particle_renderer_.render_refracting(cmd_buf_[fi], window_width_, window_height_, vp.data(),
                                             fi);
    }
    resource_icon_renderer_.render(cmd_buf_[fi], window_width_, window_height_, resource_icon_time_,
                                   fi);
}

void Renderer::record_screen_layers(u32 fi) {
    // 6. Draw strategic icons (when zoomed out, replaces 3D unit meshes)
    if (ui_pipeline_ && strategic_icon_renderer_.quad_count() > 0) {
        vkc::bind_pipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline_);
        strategic_icon_renderer_.render(cmd_buf_[fi], ui_layout_,
                                         window_width_, window_height_);
    }

    // 7. Draw game overlays (health bars, selection, command lines)
    if (ui_pipeline_ && overlay_renderer_.quad_count() > 0) {
        vkc::bind_pipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline_);
        overlay_renderer_.render(cmd_buf_[fi], ui_layout_,
                                 window_width_, window_height_);
    }

    // 8. Draw minimap (terrain bg + unit icons + camera box)
    if (legacy_hud_active_ && ui_pipeline_ && minimap_renderer_.quad_count() > 0) {
        vkc::bind_pipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline_);
        minimap_renderer_.render(cmd_buf_[fi], ui_layout_,
                                  window_width_, window_height_);
    }

    // 9. Draw economy HUD (resource bars + text at top of screen)
    if (legacy_hud_active_ && ui_pipeline_ && hud_renderer_.quad_count() > 0) {
        vkc::bind_pipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline_);
        hud_renderer_.render(cmd_buf_[fi], ui_layout_,
                              window_width_, window_height_);
    }

    // 10. Draw selection info panel (bottom-center unit details)
    if (legacy_hud_active_ && ui_pipeline_ && selection_info_renderer_.quad_count() > 0) {
        vkc::bind_pipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline_);
        selection_info_renderer_.render(cmd_buf_[fi], ui_layout_,
                                         window_width_, window_height_);
    }

    // 11. Draw UI (screen-space 2D quads, last — always on top)
    if (ui_pipeline_ && ui_renderer_.quad_count() > 0) {
        vkc::bind_pipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline_);
        ui_renderer_.render(cmd_buf_[fi], ui_layout_,
                            window_width_, window_height_);
    }

    // 12. Draw profile overlay (topmost, after all other UI)
    if (ui_pipeline_ && profile_overlay_.quad_count() > 0) {
        vkc::bind_pipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline_);
        profile_overlay_.render(cmd_buf_[fi], ui_layout_, window_width_, window_height_);
    }
}

void Renderer::submit_and_present(u32 fi, u32 image_index, bool world, bool capturing,
                                  bool scene_capturing) {
    vkEndCommandBuffer(cmd_buf_[fi]);

    // Submit
    PROFILE_ZONE("Render::submit");
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &present_semaphore_[fi];
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_buf_[fi];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_finished_[image_index];
    VK_CHECK(vkQueueSubmit(graphics_queue_, 1, &submit, render_fence_[fi]));
    if (world) {
        last_command_counts_ = take_command_counts();
        ++frame_sequence_;
    } else {
        (void)take_command_counts(); // a UI-only frame's aren't a world frame's
    }

    // Present
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_finished_[image_index];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &image_index;
    const VkResult pres_result = vkQueuePresentKHR(graphics_queue_, &present);
    if (capturing) deliver_capture();
    if (scene_capturing) deliver_scene_capture();

    if (pres_result == VK_ERROR_OUT_OF_DATE_KHR || pres_result == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain();
    }

    // Advance frame index for next frame
    frame_index_ = (frame_index_ + 1) % FRAMES_IN_FLIGHT;
}

const std::vector<std::string>& Renderer::decal_frames(const std::string& path) {
    auto it = decal_frames_.find(path);
    if (it == decal_frames_.end()) {
        it = decal_frames_
                 .emplace(path, decal_frame_names(path,
                                                  [&](const std::string& name) {
                                                      return texture_cache_.exists(name);
                                                  }))
                 .first;
        // Each frame loads now, so the first round shows them all
        if (it->second.size() > 1)
            for (const std::string& frame : it->second) (void)texture_cache_.get(frame);
    }
    return it->second;
}

const std::string* Renderer::decal_frame(const std::string& path, f32 ticks) {
    if (path.empty()) return &path;
    const std::vector<std::string>& frames = decal_frames(path);
    return &frames[decal_frame_at(ticks, frames.size())];
}

void Renderer::collect_frame_decals(const Frustum& frustum, f32 ticks) {
    frame_decals_.clear();
    if (!decals_enabled_ || !terrain_) return;
    const f32 aspect = static_cast<f32>(window_width_) / static_cast<f32>(window_height_);
    const f32 half_width = camera_.tan_half_fov_y(aspect) * aspect;
    f32 ex = 0;
    f32 ey = 0;
    f32 ez = 0;
    camera_.eye_position(ex, ey, ez);
    const std::array<f32, 3> eye = {ex, ey, ez};
    const std::array<f32, 16> view = camera_.view();
    // The map's decals are fidelity 1 (CWldTerrainDecal's load): the low
    // fidelity terrain draws none of them (M212h).
    for (const StoredDecal& sd : stored_decals_) {
        if (fidelity() == 0) break;
        const f32 ground = terrain_->get_terrain_height(sd.mid_x, sd.mid_z);
        if (!frustum.is_sphere_visible(sd.mid_x, ground, sd.mid_z, sd.radius + 64.0f)) continue;
        const f32 alpha =
            decal_lod_alpha(sd.cut_off_lod, sd.near_cut_off_lod,
                            decal_lod_metric(view, eye, half_width, sd.mid_x, ground, sd.mid_z));
        if (alpha < 1.0f / 255.0f) continue;
        // Its albedo and specular, each a frame of its sequence (CAnimTexture)
        frame_decals_.push_back({sd.technique, decal_frame(sd.albedo_path, ticks),
                                 decal_frame(sd.spec_path, ticks), sd.u, sd.v, alpha, sd.rotation_y,
                                 sd.first_index, sd.index_count, false});
    }
    // The scripts' are terrain decals too: their textures animate alike
    for (const auto& d : runtime_decals_.decal_draws())
        frame_decals_.push_back({d.technique, decal_frame(d.decal->info.texture_path, ticks),
                                 decal_frame(d.decal->info.texture2_path, ticks), d.u, d.v, d.alpha,
                                 d.decal->info.rotation_y, d.first_index, d.index_count, true});
}

void Renderer::record_decals(VkCommandBuffer cmd, u32 fi, DecalTechnique technique,
                             VkPipeline pipeline, const std::array<f32, 16>& view_proj) {
    if (!pipeline || !terrain_ || !decal_mask_ds_ || !terrain_tex_ds_ || !shadow_ds_[fi]) return;
    const bool any = std::any_of(frame_decals_.begin(), frame_decals_.end(),
                                 [&](const FrameDecal& d) { return d.technique == technique; });
    if (!any) return;
    // Every decal technique's pipeline shares decal_layout_'s sets and push
    // block.
    vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const std::array<VkDescriptorSet, 2> shared = {terrain_tex_ds_, shadow_ds_[fi]};
    vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, decal_layout_, 0,
                              static_cast<u32>(shared.size()), shared.data(), 0, nullptr);
    VkBuffer vertices = terrain_mesh_.vertex_buffer();
    const VkDeviceSize no_offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices, &no_offset);
    struct DecalPC {
        f32 view_proj[16];
        f32 u[4];
        f32 v[4];
        f32 map_alpha[4]; // the map's size, DecalAlpha, 1 for AlbedoXP
        f32 eye[4];       // the camera; a normal decal's turn (cos, sin)
    } pc{};
    static_assert(sizeof(DecalPC) == 128, "matches decal_lit's push block");
    std::memcpy(pc.view_proj, view_proj.data(), sizeof(pc.view_proj));
    pc.map_alpha[0] = static_cast<f32>(terrain_->map_width());
    pc.map_alpha[1] = static_cast<f32>(terrain_->map_height());
    pc.map_alpha[3] = technique == DecalTechnique::AlbedoXP ? 1.0f : 0.0f;
    camera_.eye_position(pc.eye[0], pc.eye[1], pc.eye[2]);
    // The water's albedo decals lie on its surface (DecalsVSWaterAlbedo)
    if (technique == DecalTechnique::WaterAlbedo) pc.eye[3] = water_renderer_.water_elevation();
    VkDescriptorSet no_spec = texture_cache_.specteam_fallback_descriptor();
    std::optional<bool> bound_runtime;
    for (const FrameDecal& d : frame_decals_) {
        if (d.technique != technique) continue;
        VkBuffer indices = d.runtime ? runtime_decals_.index_buffer(fi) : decal_indices_.buffer;
        if (!indices) continue;
        if (bound_runtime != d.runtime) {
            vkCmdBindIndexBuffer(cmd, indices, 0, VK_INDEX_TYPE_UINT32);
            bound_runtime = d.runtime;
        }
        const GPUTexture* albedo = texture_cache_.get(*d.albedo);
        if (!albedo) continue; // still loading
        const GPUTexture* spec = d.spec->empty() ? nullptr : texture_cache_.get(*d.spec);
        const std::array<VkDescriptorSet, 3> own = {
            albedo->descriptor_set, spec ? spec->descriptor_set : no_spec, decal_mask_ds_};
        vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, decal_layout_, 2,
                                  static_cast<u32>(own.size()), own.data(), 0, nullptr);
        std::memcpy(pc.u, d.u, sizeof(pc.u));
        std::memcpy(pc.v, d.v, sizeof(pc.v));
        pc.map_alpha[2] = d.alpha;
        if (technique == DecalTechnique::Normals) {
            pc.eye[0] = std::cos(d.rotation_y);
            pc.eye[1] = std::sin(d.rotation_y);
        }
        vkc::push_constants(cmd, decal_layout_,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                            sizeof(pc), &pc);
        vkc::draw_indexed(cmd, d.index_count, 1, d.first_index, 0, 0);
    }
}

void Renderer::bind_normal_target() {
    if (!terrain_tex_ds_ || !frame_.terrain_normal().view) return;
    VkDescriptorImageInfo info{};
    info.sampler = water_renderer_.clamp_sampler(); // read with texelFetch
    info.imageView = frame_.terrain_normal().view;
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = terrain_tex_ds_;
    write.dstBinding = 26;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkc::update_descriptor_sets(device_, 1, &write, 0, nullptr);
}

u32 Renderer::mesh_instance_count() const {
    u32 n = 0;
    for (const MeshDrawGroup& g : unit_renderer_.mesh_groups()) n += g.instance_count;
    return n;
}

void Renderer::init_ui_caches(vfs::VirtualFileSystem* vfs) {
    if (vfs) vfs_ = vfs;
    if (caches_initialized_ || !vfs) return;
    texture_cache_.init(device_, allocator_, cmd_pool_, graphics_queue_,
                        texture_ds_layout_, texture_sampler_, vfs);
    font_cache_.init(device_, allocator_, cmd_pool_, graphics_queue_,
                     texture_ds_layout_, texture_sampler_, vfs);
    caches_initialized_ = true;
    spdlog::info("UI caches initialized (texture + font)");
}

bool Renderer::should_close() const {
    return window_ && glfwWindowShouldClose(window_);
}

bool Renderer::is_key_pressed(int glfw_key) const {
    if (scripted_pointer_) {
        switch (glfw_key) {
        case GLFW_KEY_LEFT_SHIFT:
        case GLFW_KEY_RIGHT_SHIFT: return (scripted_pointer_->mods & GLFW_MOD_SHIFT) != 0;
        case GLFW_KEY_LEFT_CONTROL:
        case GLFW_KEY_RIGHT_CONTROL: return (scripted_pointer_->mods & GLFW_MOD_CONTROL) != 0;
        case GLFW_KEY_LEFT_ALT:
        case GLFW_KEY_RIGHT_ALT: return (scripted_pointer_->mods & GLFW_MOD_ALT) != 0;
        default: break;
        }
    }
    return window_ && glfwGetKey(window_, glfw_key) == GLFW_PRESS;
}

void Renderer::set_window_title(const char* title) {
    if (window_) glfwSetWindowTitle(window_, title);
}

void Renderer::mouse_position(f64& x, f64& y) const {
    if (scripted_pointer_) {
        x = scripted_pointer_->x;
        y = scripted_pointer_->y;
        return;
    }
    x = 0;
    y = 0;
    if (!window_) return;
    // In framebuffer pixels (M217h): the viewport's and the UI's units
    glfwGetCursorPos(window_, &x, &y);
    int ww = 0;
    int wh = 0;
    int fw = 0;
    int fh = 0;
    glfwGetWindowSize(window_, &ww, &wh);
    glfwGetFramebufferSize(window_, &fw, &fh);
    const auto p = core::to_framebuffer(x, y, ww, wh, fw, fh);
    x = p[0];
    y = p[1];
}

void Renderer::set_fullscreen(u32 width, u32 height, u32 rate) {
    if (!window_) return;
    if constexpr (core::kNativeFullscreen) {
        native_fullscreen_request_ = true;
        windowed_pending_.reset();
        settle_native_fullscreen();
        return;
    }
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (!monitor) return;
    glfwSetWindowMonitor(window_, monitor, 0, 0, static_cast<int>(width), static_cast<int>(height),
                         rate > 0 ? static_cast<int>(rate) : GLFW_DONT_CARE);
}

void Renderer::set_windowed(u32 width, u32 height, std::optional<std::array<i32, 2>> position,
                            bool maximized) {
    if (!window_) return;
    const WindowedPlace place{width, height, position, maximized};
    if constexpr (core::kNativeFullscreen) {
        native_fullscreen_request_ = false;
        windowed_pending_ = place;
        settle_native_fullscreen();
        return;
    }
    apply_windowed(place);
}

void Renderer::settle_native_fullscreen() {
#ifdef __APPLE__
    const auto step = core::native_fullscreen_step(
        native_fullscreen_request_, platform::native_fullscreen(window_),
        platform::native_fullscreen_moving(window_), windowed_pending_.has_value());
    if (step.toggle) {
        platform::toggle_native_fullscreen(window_);
    }
    if (step.settled) {
        native_fullscreen_request_.reset();
    }
    if (step.apply_windowed) {
        const WindowedPlace place = *windowed_pending_;
        windowed_pending_.reset();
        apply_windowed(place);
    }
#endif
}

void Renderer::apply_windowed(const WindowedPlace& place) {
    const u32 width = place.width;
    const u32 height = place.height;
    int x = 0;
    int y = 0;
    if (place.position) {
        x = (*place.position)[0];
        y = (*place.position)[1];
    } else if (glfwGetWindowMonitor(window_)) {
        // Out of full screen with no place: the middle of the display
        if (const GLFWvidmode* mode = glfwGetVideoMode(glfwGetPrimaryMonitor())) {
            x = std::max(0, (mode->width - static_cast<int>(width)) / 2);
            y = std::max(0, (mode->height - static_cast<int>(height)) / 2);
        }
    } else {
        glfwGetWindowPos(window_, &x, &y);
    }
    if (glfwGetWindowAttrib(window_, GLFW_MAXIMIZED)) glfwRestoreWindow(window_);
    glfwSetWindowMonitor(window_, nullptr, x, y, static_cast<int>(width), static_cast<int>(height),
                         GLFW_DONT_CARE);
    glfwSetWindowAttrib(window_, GLFW_DECORATED, GLFW_TRUE);
    if (place.maximized) glfwMaximizeWindow(window_);
}

bool Renderer::fullscreen() const {
#ifdef __APPLE__
    return window_ && (windowed_pending_ ||
                       native_fullscreen_request_.value_or(platform::native_fullscreen(window_)));
#else
    return window_ && glfwGetWindowMonitor(window_) != nullptr;
#endif
}

std::vector<std::array<u32, 3>> Renderer::display_modes() const {
    std::vector<std::array<u32, 3>> out;
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (!monitor) return out;
    int count = 0;
    const GLFWvidmode* modes = glfwGetVideoModes(monitor, &count);
    for (int i = 0; modes && i < count; ++i) {
        out.push_back({static_cast<u32>(modes[i].width), static_cast<u32>(modes[i].height),
                       static_cast<u32>(modes[i].refreshRate)});
    }
    return out;
}

std::optional<Renderer::WindowGeometry> Renderer::windowed_geometry() const {
    if (!window_ || fullscreen()) return std::nullopt;
    WindowGeometry g;
    int w = 0;
    int h = 0;
    glfwGetWindowPos(window_, &g.x, &g.y);
    glfwGetWindowSize(window_, &w, &h);
    g.width = static_cast<u32>(std::max(w, 0));
    g.height = static_cast<u32>(std::max(h, 0));
    g.maximized = glfwGetWindowAttrib(window_, GLFW_MAXIMIZED) != 0;
    return g;
}

void Renderer::set_cursor_clip(bool on) {
    // Moho clips only a windowed head (ClipCursor to its rect): GLFW's
    // captured cursor, which shows; the system's stays hidden while the
    // engine draws FA's
    cursor_clipped_ = on && window_ && !fullscreen();
    if (window_)
        glfwSetInputMode(window_, GLFW_CURSOR,
                         cursor_clipped_      ? GLFW_CURSOR_CAPTURED
                         : system_cursor_set_ ? GLFW_CURSOR_NORMAL
                                              : GLFW_CURSOR_HIDDEN);
}

void Renderer::update_system_cursor(lua_State* L) {
    // Moho's cursor is the system's: drawn by the OS, it keeps up with the
    // mouse and the desktop's own effects (KDE's shake-to-find enlarged a
    // hidden one, black). A capture has no OS cursor to show it.
    if (offscreen_ || !window_ || !L) return;
    std::string path;
    f32 hot_x = 0;
    f32 hot_y = 0;
    lua_pushstring(L, "__osc_active_cursor");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "_c_object");
        lua_rawget(L, -2);
        const auto* cursor = static_cast<const ui::UIControl*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        if (cursor && cursor->cursor_visible()) {
            path = cursor->cursor_texture();
            hot_x = cursor->cursor_hotspot_x();
            hot_y = cursor->cursor_hotspot_y();
        }
    }
    lua_pop(L, 1);
    const std::string key =
        path.empty() ? std::string() : fmt::format("{}|{}|{}", path, hot_x, hot_y);
    if (key == system_cursor_key_ && (key.empty() || system_cursor_set_)) return;
    system_cursor_key_ = key;
    GLFWcursor* made = nullptr;
    if (!key.empty()) {
        if (auto it = system_cursors_.find(key); it != system_cursors_.end()) {
            made = it->second;
        } else if (vfs_) {
            if (const auto file = vfs_->read_file(path)) {
                u32 w = 0;
                u32 h = 0;
                if (auto rgba = dds_to_rgba(*file, w, h)) {
                    GLFWimage image{static_cast<int>(w), static_cast<int>(h), rgba->data()};
                    made =
                        glfwCreateCursor(&image, static_cast<int>(hot_x), static_cast<int>(hot_y));
                }
            }
            system_cursors_[key] = made; // a failure too: drawn by the engine
        }
    }
    system_cursor_set_ = made != nullptr;
    // Without one (none set, or its image not to be made), the engine draws it
    ui_renderer_.set_draw_cursor(!system_cursor_set_);
    if (made) glfwSetCursor(window_, made);
    set_cursor_clip(cursor_clipped_);
}

bool Renderer::system_cursor_shown() const {
    return window_ && glfwGetInputMode(window_, GLFW_CURSOR) == GLFW_CURSOR_NORMAL;
}

void Renderer::set_vsync(bool on) {
    if (vsync_ == on) return;
    vsync_ = on;
    swapchain_stale_ = true;
}

bool Renderer::is_mouse_pressed(int glfw_button) const {
    if (scripted_pointer_) {
        return glfw_button >= 0 && glfw_button < 32 &&
               (scripted_pointer_->buttons & (1u << glfw_button)) != 0;
    }
    return window_ && glfwGetMouseButton(window_, glfw_button) == GLFW_PRESS;
}

void Renderer::poll_events(f64 dt) {
    if (!window_) return;
    glfwPollEvents();
    settle_native_fullscreen();

    // The world view's camera, before input picks this frame: its moves,
    // then its basis (a pan has moved the target)
    camera_.set_viewport(static_cast<f32>(window_width_), static_cast<f32>(window_height_));
    // Its clocks: the system's, and the game's
    camera_.set_clocks(glfwGetTime(), camera_game_time_);
    camera_.update(window_, dt);
}

void Renderer::write_bone_descriptor(u32 fi) {
    VkDescriptorBufferInfo buf_info{};
    buf_info.buffer = unit_renderer_.bone_ssbo_buffer(fi);
    buf_info.offset = 0;
    buf_info.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = bone_ds_[fi];
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &buf_info;
    vkc::update_descriptor_sets(device_, 1, &write, 0, nullptr);
    bone_ds_generation_[fi] = unit_renderer_.bone_ssbo_generation(fi);
}

void Renderer::bind_mesh_environment(const map::ScmapEnvironment& environment) {
    // Each technique reflects the map's cube named by its "environment"
    // annotation -- "<default>", "<aeon>", "<seraphim>" -- falling back to the
    // map's "<default>", as Moho's GetEnvLookup does; Moho's own default cube
    // without one, and black if that fails too.
    const auto find = [&](const char* key) -> std::string {
        for (const auto& [name, file] : environment.cubemaps)
            if (name == key && !file.empty()) return file;
        return {};
    };
    std::string fallback = find("<default>");
    if (fallback.empty()) fallback = "/textures/environment/defaultenvcube.dds";
    const auto cube = [&](const char* key) {
        std::string path = find(key);
        if (path.empty()) path = fallback;
        VkImageView view = texture_cache_.get_cube_blocking(path);
        if (!view) view = texture_cache_.cube_fallback_view();
        spdlog::info("Environment cubemap {}: {}", key, path);
        return view;
    };
    const std::array<VkImageView, 3> cubes = {cube("<default>"), cube("<aeon>"),
                                              cube("<seraphim>")};
    // FA's fixed lookups (Moho binds them for every mesh).
    const auto lookup = [&](const char* path) {
        const GPUTexture* tex = texture_cache_.get_blocking(path);
        return tex ? tex->image.view : texture_cache_.fallback_view();
    };
    const std::array<VkImageView, 2> lookups = {lookup("/textures/engine/anisotropiclookup.dds"),
                                                lookup("/textures/engine/insectlookup.dds")};
    for (VkImageView v : cubes)
        if (!v) return;
    for (u32 f = 0; f < FRAMES_IN_FLIGHT; ++f) {
        if (!shadow_ds_[f]) continue;
        std::array<VkDescriptorImageInfo, 5> infos{};
        std::array<VkWriteDescriptorSet, 5> writes{};
        for (u32 i = 0; i < 5; ++i) {
            infos[i].sampler = i < 3 ? texture_sampler_ : lookup_sampler_;
            infos[i].imageView = i < 3 ? cubes[i] : lookups[i - 3];
            infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = shadow_ds_[f];
            writes[i].dstBinding = 2 + i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &infos[i];
        }
        vkc::update_descriptor_sets(device_, static_cast<u32>(writes.size()), writes.data(), 0,
                                    nullptr);
    }
}

MeshTechnique Renderer::mesh_technique(const std::string& blueprint_id, lua_State* L) {
    const GPUMesh* mesh = mesh_cache_.get(blueprint_id, L);
    return mesh ? mesh->technique : MeshTechnique::Unit;
}

// --- Vulkan validation messages ---

std::atomic<u32> Renderer::validation_errors_{0};

VkBool32 VKAPI_CALL Renderer::vulkan_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user*/) {
    const char* msg = data && data->pMessage ? data->pMessage : "(no message)";
    const char* id = data && data->pMessageIdName ? data->pMessageIdName : "-";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validation_errors_;
        spdlog::error("Vulkan validation [{}]: {}", id, msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        spdlog::warn("Vulkan validation [{}]: {}", id, msg);
    }
    return VK_FALSE; // never abort the call
}

// --- Frame capture (screenshots / golden images) ---

bool Renderer::request_capture(CaptureCallback on_captured) {
    if (!capture_supported_) return false;
    pending_capture_ = std::move(on_captured);
    return true;
}

bool Renderer::record_capture(VkCommandBuffer cmd, u32 image_index) {
    if (!pending_capture_ || !capture_supported_ ||
        image_index >= swapchain_images_.size()) {
        return false;
    }

    const VkDeviceSize size =
        static_cast<VkDeviceSize>(window_width_) * window_height_ * 4;
    if (capture_buf_size_ != size) {
        if (capture_buf_.buffer) {
            vmaDestroyBuffer(allocator_, capture_buf_.buffer, capture_buf_.allocation);
            capture_buf_ = {};
        }
        VkBufferCreateInfo buf_ci{};
        buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size = size;
        buf_ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
        if (vmaCreateBuffer(allocator_, &buf_ci, &alloc_ci, &capture_buf_.buffer,
                            &capture_buf_.allocation, nullptr) != VK_SUCCESS) {
            spdlog::error("Frame capture: failed to create readback buffer");
            capture_buf_ = {};
            capture_buf_size_ = 0;
            pending_capture_ = nullptr;
            return false;
        }
        capture_buf_size_ = size;
    }

    VkImage image = swapchain_images_[image_index];
    VkImageMemoryBarrier to_transfer{};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; // final pass layout
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = image;
    to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_transfer);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {window_width_, window_height_, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           capture_buf_.buffer, 1, &region);

    VkImageMemoryBarrier to_present = to_transfer;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_present);
    return true;
}

void Renderer::deliver_capture() {
    // Capture is a test/debug path; stalling the queue keeps it simple.
    vkQueueWaitIdle(graphics_queue_);

    ImageRGBA8 image;
    image.width = window_width_;
    image.height = window_height_;
    image.pixels.resize(static_cast<size_t>(capture_buf_size_));

    void* mapped = nullptr;
    if (vmaMapMemory(allocator_, capture_buf_.allocation, &mapped) != VK_SUCCESS) {
        spdlog::error("Frame capture: failed to map readback buffer");
        pending_capture_ = nullptr;
        return;
    }
    vmaInvalidateAllocation(allocator_, capture_buf_.allocation, 0, VK_WHOLE_SIZE);
    std::memcpy(image.pixels.data(), mapped, image.pixels.size());
    vmaUnmapMemory(allocator_, capture_buf_.allocation);

    const bool bgra = swapchain_format_ == VK_FORMAT_B8G8R8A8_UNORM ||
                      swapchain_format_ == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0; i < image.pixels.size(); i += 4) {
        if (bgra) std::swap(image.pixels[i], image.pixels[i + 2]);
        image.pixels[i + 3] = 255; // swapchain alpha is meaningless
    }

    auto callback = std::move(pending_capture_);
    pending_capture_ = nullptr;
    callback(std::move(image));
}

bool Renderer::record_scene_capture(VkCommandBuffer cmd) {
    if (!pending_scene_capture_ || !frame_.scene_color().image) return false;
    const VkDeviceSize size = static_cast<VkDeviceSize>(window_width_) * window_height_ * 8;
    if (scene_capture_buf_size_ != size) {
        if (scene_capture_buf_.buffer)
            vmaDestroyBuffer(allocator_, scene_capture_buf_.buffer, scene_capture_buf_.allocation);
        scene_capture_buf_ = {};
        scene_capture_buf_size_ = 0;
        VkBufferCreateInfo buf_ci{};
        buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size = size;
        buf_ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
        if (vmaCreateBuffer(allocator_, &buf_ci, &alloc_ci, &scene_capture_buf_.buffer,
                            &scene_capture_buf_.allocation, nullptr) != VK_SUCCESS) {
            spdlog::error("Scene capture: failed to create readback buffer");
            pending_scene_capture_ = nullptr;
            return false;
        }
        scene_capture_buf_size_ = size;
    }
    // The finished scene (SHADER_READ_ONLY, as the bloom and the composite
    // read it) copied out, and back for them.
    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = frame_.scene_color().image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {window_width_, window_height_, 1};
    vkCmdCopyImageToBuffer(cmd, frame_.scene_color().image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           scene_capture_buf_.buffer, 1, &region);
    VkImageMemoryBarrier back = to_src;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &back);
    return true;
}

void Renderer::deliver_scene_capture() {
    // A test path: stalling the queue keeps it simple.
    vkQueueWaitIdle(graphics_queue_);
    SceneImage image;
    image.width = window_width_;
    image.height = window_height_;
    const size_t count = static_cast<size_t>(window_width_) * window_height_ * 4;
    image.rgba.resize(count);
    void* mapped = nullptr;
    if (vmaMapMemory(allocator_, scene_capture_buf_.allocation, &mapped) != VK_SUCCESS) {
        spdlog::error("Scene capture: failed to map readback buffer");
        pending_scene_capture_ = nullptr;
        return;
    }
    vmaInvalidateAllocation(allocator_, scene_capture_buf_.allocation, 0, VK_WHOLE_SIZE);
    // R16G16B16A16_SFLOAT: each half widened to a float.
    const auto* halves = static_cast<const u16*>(mapped);
    for (size_t i = 0; i < count; ++i) {
        const u32 h = halves[i];
        const u32 exponent = (h >> 10) & 0x1Fu;
        const u32 mantissa = h & 0x3FFu;
        f32 value = 0.0f;
        if (exponent == 0) {
            value = std::ldexp(static_cast<f32>(mantissa), -24); // subnormal
        } else if (exponent == 31) {
            value = mantissa != 0 ? std::numeric_limits<f32>::quiet_NaN()
                                  : std::numeric_limits<f32>::infinity();
        } else {
            value =
                std::ldexp(static_cast<f32>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
        }
        image.rgba[i] = (h & 0x8000u) != 0 ? -value : value;
    }
    vmaUnmapMemory(allocator_, scene_capture_buf_.allocation);
    auto callback = std::move(pending_scene_capture_);
    pending_scene_capture_ = nullptr;
    callback(std::move(image));
}

bool Renderer::begin_frame_slot(u32 fi, u32& image_index) {
    // Never an endless wait: a window the compositor isn't showing (alt-tabbed
    // away on NVIDIA's Wayland driver, where FIFO presentation waits on the
    // compositor) can get no image back, nor its last frame's semaphore
    // signalled, for as long as it stays hidden. Waiting forever froze the
    // game's loop with it -- its input, its sim and a network game's
    // lockstep. A frame that can't start is skipped instead.
    constexpr u64 kWaitNs = 100'000'000; // 0.1 s
    if (vkWaitForFences(device_, 1, &render_fence_[fi], VK_TRUE, kWaitNs) != VK_SUCCESS) {
        return false;
    }
    // The GPU's figures for the frame that last used this slot (M223b)
    gpu_queries_.collect(device_, fi);

    // A resized window or a vsync change: a new swapchain first (M217h)
    if (swapchain_stale_) recreate_swapchain();
    if (swapchain_stale_) return false; // minimized: no surface to draw to

    const VkResult acquired = vkAcquireNextImageKHR(
        device_, swapchain_, kWaitNs, present_semaphore_[fi], VK_NULL_HANDLE, &image_index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        return false;
    }
    return acquired == VK_SUCCESS || acquired == VK_SUBOPTIMAL_KHR;
}

void Renderer::recreate_swapchain() {
    swapchain_stale_ = false;
    // A minimized window (a zero-sized framebuffer, on X11) has nothing to
    // draw to: keep the old swapchain and try again on a later frame,
    // without waiting here -- the game runs on while minimized.
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    if (w == 0 || h == 0) {
        swapchain_stale_ = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return;
    }

    vkDeviceWaitIdle(device_);

    // Destroy the frame's targets (they depend on window size)
    destroy_frame_targets();

    // Destroy old framebuffers and depth image
    for (auto fb : framebuffers_)
        vkDestroyFramebuffer(device_, fb, nullptr);
    framebuffers_.clear();

    vkDestroyImageView(device_, depth_image_.view, nullptr);
    vmaDestroyImage(allocator_, depth_image_.image, depth_image_.allocation);

    for (auto iv : swapchain_image_views_)
        vkDestroyImageView(device_, iv, nullptr);

    // Save old swapchain handle (create_swapchain overwrites swapchain_)
    VkSwapchainKHR old_sc = swapchain_;

    // Recreate
    if (!create_swapchain(static_cast<u32>(w), static_cast<u32>(h))) {
        spdlog::error("recreate_swapchain: create_swapchain failed");
        swapchain_ = VK_NULL_HANDLE;
        return;
    }

    // Destroy retired swapchain (spec requires explicit destroy after recreation)
    if (old_sc != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(device_, old_sc, nullptr);
    create_depth_image();
    create_framebuffers();

    // Recreate the frame's targets at new resolution
    create_frame_targets();
    bloom_.create_pipelines(render_pass_, texture_ds_layout_);
}

void Renderer::shutdown() {
    if (!initialized_) return;

    vkDeviceWaitIdle(device_);

    // Terrain texture pool first (references image views owned by texture_cache_)
    if (terrain_tex_ds_pool_)
        vkDestroyDescriptorPool(device_, terrain_tex_ds_pool_, nullptr);
    destroy_terrain_strata_ubo();
    if (terrain_tex_ds_layout_)
        vkDestroyDescriptorSetLayout(device_, terrain_tex_ds_layout_, nullptr);

    // Sub-renderers
    terrain_mesh_.destroy(device_, allocator_);
    unit_renderer_.destroy(device_, allocator_);
    water_renderer_.destroy(device_, allocator_);
    sky_renderer_.destroy(device_, allocator_);
    fog_renderer_.destroy(device_, allocator_);
    movie_textures_.destroy();
    ui_renderer_.destroy(device_, allocator_);
    overlay_renderer_.destroy(device_, allocator_);
    particle_renderer_.destroy(device_, allocator_);
    runtime_decals_.destroy(device_, allocator_);
    beam_renderer_.destroy(device_, allocator_);
    command_graph_renderer_.destroy(device_, allocator_);
    selection_renderer_.destroy(device_, allocator_);
    range_renderer_.destroy(device_, allocator_);
    resource_icon_renderer_.destroy(device_, allocator_);
    gpu_queries_.destroy(device_);
    trail_renderer_.destroy(device_, allocator_);
    minimap_renderer_.destroy(device_, allocator_);
    strategic_icon_renderer_.destroy(device_, allocator_);
    hud_renderer_.destroy(device_, allocator_);
    selection_info_renderer_.destroy(device_, allocator_);
    profile_overlay_.destroy(device_, allocator_);
    font_cache_.destroy(device_, allocator_);
    mesh_cache_.destroy(device_, allocator_);
    texture_cache_.destroy(device_, allocator_);

    // Bone SSBO infrastructure
    if (bone_ds_pool_)
        vkDestroyDescriptorPool(device_, bone_ds_pool_, nullptr);
    if (bone_ds_layout_)
        vkDestroyDescriptorSetLayout(device_, bone_ds_layout_, nullptr);

    // Texture infrastructure
    if (texture_sampler_) vkDestroySampler(device_, texture_sampler_, nullptr);
    if (lookup_sampler_) vkDestroySampler(device_, lookup_sampler_, nullptr);
    if (texture_ds_layout_)
        vkDestroyDescriptorSetLayout(device_, texture_ds_layout_, nullptr);

    destroy_decal_buffers();

    // Shadow infrastructure
    if (shadow_ds_pool_)
        vkDestroyDescriptorPool(device_, shadow_ds_pool_, nullptr);
    if (shadow_ds_layout_)
        vkDestroyDescriptorSetLayout(device_, shadow_ds_layout_, nullptr);
    shadow_casters_.destroy();
    shadow_map_.destroy();
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        if (light_ubo_[i].buffer)
            vmaDestroyBuffer(allocator_, light_ubo_[i].buffer, light_ubo_[i].allocation);
    }

    // The frame's targets
    destroy_frame_targets();

    // Pipelines
    vkDestroyPipeline(device_, terrain_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, terrain_layout_, nullptr);
    if (terrain_low_pipeline_) vkDestroyPipeline(device_, terrain_low_pipeline_, nullptr);
    if (terrain_low_layout_) vkDestroyPipelineLayout(device_, terrain_low_layout_, nullptr);
    if (terrain_skirt_pipeline_) {
        vkDestroyPipeline(device_, terrain_skirt_pipeline_, nullptr);
    }
    if (terrain_skirt_layout_) {
        vkDestroyPipelineLayout(device_, terrain_skirt_layout_, nullptr);
    }
    vkDestroyPipeline(device_, unit_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, unit_layout_, nullptr);
    vkDestroyPipeline(device_, mesh_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, mesh_layout_, nullptr);
    vkDestroyPipeline(device_, mesh_fade_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, mesh_fade_layout_, nullptr);
    vkDestroyPipeline(device_, mesh_overlay_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, mesh_overlay_layout_, nullptr);
    vkDestroyPipeline(device_, mesh_cube_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, mesh_cube_layout_, nullptr);
    vkDestroyPipeline(device_, mesh_feedback_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, mesh_feedback_layout_, nullptr);
    for (u32 i = 0; i < kShieldStates; ++i) {
        vkDestroyPipeline(device_, shield_pipelines_[i], nullptr);
        vkDestroyPipelineLayout(device_, shield_layouts_[i], nullptr);
    }
    vkDestroyPipeline(device_, decal_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, decal_layout_, nullptr);
    for (VkPipeline pipeline :
         {decal_glow_pipeline_, decal_glow_mask_pipeline_, decal_normal_pipeline_,
          terrain_normal_pipeline_, decal_water_pipeline_})
        if (pipeline) vkDestroyPipeline(device_, pipeline, nullptr);
    for (VkPipelineLayout layout :
         {decal_glow_layout_, decal_glow_mask_layout_, decal_normal_layout_, terrain_normal_layout_,
          decal_water_layout_})
        if (layout) vkDestroyPipelineLayout(device_, layout, nullptr);
    if (ui_pipeline_) vkDestroyPipeline(device_, ui_pipeline_, nullptr);
    if (ui_layout_) vkDestroyPipelineLayout(device_, ui_layout_, nullptr);

    // Sync (per-frame)
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        vkDestroyFence(device_, render_fence_[i], nullptr);
        vkDestroySemaphore(device_, present_semaphore_[i], nullptr);
    }
    for (auto sem : render_finished_) vkDestroySemaphore(device_, sem, nullptr);
    render_finished_.clear();

    // Command pool
    vkDestroyCommandPool(device_, cmd_pool_, nullptr);

    // Framebuffers
    for (auto fb : framebuffers_)
        vkDestroyFramebuffer(device_, fb, nullptr);

    // Render pass
    vkDestroyRenderPass(device_, render_pass_, nullptr);

    // Depth image
    vkDestroyImageView(device_, depth_image_.view, nullptr);
    vmaDestroyImage(allocator_, depth_image_.image, depth_image_.allocation);

    // Swapchain image views
    for (auto iv : swapchain_image_views_)
        vkDestroyImageView(device_, iv, nullptr);

    // Swapchain
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);

    if (scene_capture_buf_.buffer) {
        vmaDestroyBuffer(allocator_, scene_capture_buf_.buffer, scene_capture_buf_.allocation);
        scene_capture_buf_ = {};
    }
    if (capture_buf_.buffer) {
        vmaDestroyBuffer(allocator_, capture_buf_.buffer, capture_buf_.allocation);
        capture_buf_ = {};
    }

    // VMA
    vmaDestroyAllocator(allocator_);

    // Device & instance
    vkDestroyDevice(device_, nullptr);
    vkDestroySurfaceKHR(instance_, surface_, nullptr);

    vkb::destroy_debug_utils_messenger(instance_, debug_messenger_);
    vkDestroyInstance(instance_, nullptr);

    // GLFW
    for (auto& [key, cursor] : system_cursors_)
        if (cursor) glfwDestroyCursor(cursor);
    system_cursors_.clear();
    glfwDestroyWindow(window_);
    glfwTerminate();

    initialized_ = false;
    spdlog::info("Renderer shut down");
}

} // namespace osc::renderer
